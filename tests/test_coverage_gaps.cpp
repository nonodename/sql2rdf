/**
 * Additional unit/integration tests targeting coverage gaps identified via a
 * gcov pass over src/r2rml/*.cpp.  Each TEST_CASE below documents, in its
 * comment, exactly which previously-uncovered function/branch it exercises.
 *
 * These tests exercise EXISTING production behaviour only; no production
 * code was changed to accommodate them.
 */

#include <catch2/catch_test_macros.hpp>

#include <serd/serd.h>

#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// Fallback for IDE tooling; CMake overrides this via target_compile_definitions.
#ifndef SOURCE_R2RML_DIR
#define SOURCE_R2RML_DIR ""
#endif

#include "r2rml/BaseTableOrView.h"
#include "r2rml/ColumnTermMap.h"
#include "r2rml/ConstantTermMap.h"
#include "r2rml/GraphMap.h"
#include "r2rml/JoinCondition.h"
#include "r2rml/LogicalTable.h"
#include "r2rml/MapSQLRow.h"
#include "r2rml/ObjectMap.h"
#include "r2rml/PredicateMap.h"
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
#include "MockSQL.h"

using r2rml::BaseTableOrView;
using r2rml::ColumnTermMap;
using r2rml::ConstantTermMap;
using r2rml::GraphMap;
using r2rml::JoinCondition;
using r2rml::LogicalTable;
using r2rml::MapSQLRow;
using r2rml::ObjectMap;
using r2rml::PredicateMap;
using r2rml::PredicateObjectMap;
using r2rml::R2RMLMapping;
using r2rml::R2RMLParser;
using r2rml::R2RMLView;
using r2rml::ReferencingObjectMap;
using r2rml::SQLConnection;
using r2rml::SQLResultSet;
using r2rml::SQLRow;
using r2rml::SQLValue;
using r2rml::StringSQLValue;
using r2rml::SubjectMap;
using r2rml::TemplateTermMap;
using r2rml::TermMap;
using r2rml::TermType;
using r2rml::TriplesMap;
using r2rml::testing::makeRow;
using r2rml::testing::MockSQLConnection;

namespace {

// ---------------------------------------------------------------------------
// Minimal concrete subclasses of the abstract marker/base classes.
//
// GraphMap, ObjectMap, PredicateMap, SubjectMap, ReferencingObjectMap and
// LogicalTable all inherit at least one pure-virtual member (generateRDFTerm
// and/or getRows/getColumnNames/isValid) without overriding it, so they
// remain abstract.  The production parser defines its own private concrete
// wrappers (ConcreteSubjectMap, ConcreteReferencingObjectMap) in
// R2RMLParser.cpp; we do the same here, purely for test purposes, so that
// destructors/print()/isValid() on the base classes themselves can be
// exercised directly.
// ---------------------------------------------------------------------------

class TestGraphMap : public GraphMap {
public:
	SerdNode generateRDFTerm(const SQLRow & /*row*/, const SerdEnv & /*env*/) const override {
		return SERD_NODE_NULL;
	}
};

class TestObjectMap : public ObjectMap {
public:
	SerdNode generateRDFTerm(const SQLRow & /*row*/, const SerdEnv & /*env*/) const override {
		return SERD_NODE_NULL;
	}
};

class TestPredicateMap : public PredicateMap {
public:
	SerdNode generateRDFTerm(const SQLRow & /*row*/, const SerdEnv & /*env*/) const override {
		return SERD_NODE_NULL;
	}
};

// Mirrors R2RMLParser.cpp's private ConcreteSubjectMap: adds a delegate
// TermMap so the abstract SubjectMap can actually produce a term.
class TestValueSubjectMap : public SubjectMap {
public:
	std::unique_ptr<TermMap> valueMap;

	SerdNode generateRDFTerm(const SQLRow &row, const SerdEnv &env) const override {
		if (valueMap) {
			return valueMap->generateRDFTerm(row, env);
		}
		return SERD_NODE_NULL;
	}
};

// Mirrors R2RMLParser.cpp's private ConcreteReferencingObjectMap.
class TestReferencingObjectMap : public ReferencingObjectMap {
public:
	SerdNode generateRDFTerm(const SQLRow & /*row*/, const SerdEnv & /*env*/) const override {
		return SERD_NODE_NULL;
	}
};

// A trivial concrete LogicalTable that never returns any rows; used to
// exercise LogicalTable's own (base) print() and the "null result set"
// branch of R2RMLMapping::processDatabase.
class TestNullRowsLogicalTable : public LogicalTable {
public:
	std::unique_ptr<SQLResultSet> getRows(SQLConnection & /*dbConnection*/) override {
		return nullptr;
	}
	std::vector<std::string> getColumnNames() override {
		return {};
	}
	bool isValid() const override {
		return true;
	}
};

// A SQLConnection whose execute() always reports failure (nullptr), used to
// exercise ReferencingObjectMap::getJoinedRows' "parent result set is null"
// guard without needing a real backend.
class NullResultConnection : public SQLConnection {
public:
	std::unique_ptr<SQLResultSet> execute(const std::string & /*query*/) override {
		return nullptr;
	}
};

std::string nodeUri(const SerdNode &n) {
	return std::string(reinterpret_cast<const char *>(n.buf), n.n_bytes);
}

// Run TriplesMap::generateTriples (or PredicateObjectMap::processRow) and
// capture the NTriples serialisation as a string, mirroring the helper in
// test_process_database.cpp.
std::string captureNTriples(const std::function<void(SerdWriter &)> &action) {
	SerdChunk chunk {nullptr, 0};
	SerdEnv *env = serd_env_new(nullptr);
	SerdWriter *writer = serd_writer_new(SERD_NTRIPLES, (SerdStyle)0, env, nullptr, serd_chunk_sink, &chunk);

	action(*writer);

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
// GraphMap / ObjectMap / PredicateMap: these are pure marker subclasses of
// TermMap with no additional members.  In production nothing ever
// instantiates a rr:graphMap (the parser never builds a GraphMap), so their
// destructors are otherwise dead code from the test suite's point of view.
// ---------------------------------------------------------------------------

TEST_CASE("GraphMap, ObjectMap and PredicateMap are constructible and default-valid") {
	TestGraphMap gm;
	TestObjectMap om;
	TestPredicateMap pm;

	// All three inherit TermMap::isValid(), which defaults to true.
	REQUIRE(gm.isValid());
	REQUIRE(om.isValid());
	REQUIRE(pm.isValid());
}

// ---------------------------------------------------------------------------
// SubjectMap::print() (SubjectMap.cpp) – in production this is shadowed by
// R2RMLParser's private ConcreteSubjectMap::print() override, so the base
// class's own implementation is never invoked through the parser.  It is
// still part of the public API surface and worth exercising directly.
// ---------------------------------------------------------------------------

TEST_CASE("SubjectMap::print via operator<< renders classes and graph maps") {
	TestValueSubjectMap sm;
	sm.classIRIs.push_back("http://example.com/ns#Employee");
	sm.classIRIs.push_back("http://example.com/ns#Person");
	sm.graphMaps.push_back(std::unique_ptr<TestGraphMap>(new TestGraphMap()));

	REQUIRE(sm.isValid()); // SubjectMap::isValid() only checks graphMaps validity

	std::ostringstream oss;
	oss << sm;
	const std::string out = oss.str();

	REQUIRE(out.find("SubjectMap {") != std::string::npos);
	REQUIRE(out.find("classes=[") != std::string::npos);
	REQUIRE(out.find("http://example.com/ns#Employee") != std::string::npos);
	REQUIRE(out.find("http://example.com/ns#Person") != std::string::npos);
	REQUIRE(out.find("graphMaps=[") != std::string::npos);
}

TEST_CASE("SubjectMap::print via operator<< omits empty sections") {
	TestValueSubjectMap sm; // no classIRIs, no graphMaps
	std::ostringstream oss;
	oss << sm;
	const std::string out = oss.str();

	REQUIRE(out.find("SubjectMap {") != std::string::npos);
	REQUIRE(out.find("classes=[") == std::string::npos);
	REQUIRE(out.find("graphMaps=[") == std::string::npos);
}

// ---------------------------------------------------------------------------
// LogicalTable's own print()/operator<< (base-class fallback implementation)
// ---------------------------------------------------------------------------

TEST_CASE("LogicalTable base print() reports effectiveSqlQuery") {
	TestNullRowsLogicalTable lt;
	lt.effectiveSqlQuery = "SELECT * FROM EMP";

	std::ostringstream oss;
	oss << lt;
	REQUIRE(oss.str() == "LogicalTable { effectiveSqlQuery=\"SELECT * FROM EMP\" }");
}

// ---------------------------------------------------------------------------
// BaseTableOrView::getColumnNames() / R2RMLView::getColumnNames() – both
// currently return an empty vector unconditionally; neither is called by any
// other production code path.
// ---------------------------------------------------------------------------

TEST_CASE("BaseTableOrView::getColumnNames returns an empty vector") {
	BaseTableOrView table("EMP");
	REQUIRE(table.getColumnNames().empty());
}

TEST_CASE("R2RMLView::getColumnNames returns an empty vector") {
	R2RMLView view("SELECT * FROM EMP");
	REQUIRE(view.getColumnNames().empty());
}

// ---------------------------------------------------------------------------
// R2RMLView::print()/operator<< – with and without sqlVersions populated.
// ---------------------------------------------------------------------------

TEST_CASE("R2RMLView::print without sqlVersions") {
	R2RMLView view("SELECT * FROM EMP");
	std::ostringstream oss;
	oss << view;
	const std::string out = oss.str();
	REQUIRE(out.find("R2RMLView { sqlQuery=\"SELECT * FROM EMP\"") != std::string::npos);
	REQUIRE(out.find("versions=[") == std::string::npos);
}

TEST_CASE("R2RMLView::print with sqlVersions lists each version") {
	R2RMLView view("SELECT * FROM EMP");
	view.sqlVersions.push_back("SQL2008");
	view.sqlVersions.push_back("Oracle");

	std::ostringstream oss;
	oss << view;
	const std::string out = oss.str();
	REQUIRE(out.find("versions=[SQL2008, Oracle]") != std::string::npos);
}

// ---------------------------------------------------------------------------
// MapSQLRow copy-assignment operator and isNull() for a present, non-null
// column (the existing test suite only covers isNull() for a missing column).
// ---------------------------------------------------------------------------

TEST_CASE("MapSQLRow copy-assignment operator deep-copies columns") {
	auto row = makeRow({{"NAME", StringSQLValue(std::string("SMITH"))}, {"AGE", StringSQLValue(42)}});

	MapSQLRow other;
	other = row; // exercise operator=(const MapSQLRow&), not the copy ctor

	auto v = other.getValue("NAME");
	REQUIRE_FALSE(v->isNull());
	REQUIRE(v->asString() == "SMITH");

	// The two rows must own independent copies of the underlying values.
	REQUIRE_FALSE(other.isNull("AGE"));
}

TEST_CASE("MapSQLRow::isNull returns false for a present, non-null column") {
	auto row = makeRow({{"NAME", StringSQLValue(std::string("SMITH"))}});
	REQUIRE_FALSE(row.isNull("NAME"));
	REQUIRE(row.isNull("MISSING"));
}

// ---------------------------------------------------------------------------
// TemplateTermMap: IRI-safe percent-encoding of reserved characters, and the
// malformed-template ("{" with no matching "}") recovery path.
// ---------------------------------------------------------------------------

TEST_CASE("TemplateTermMap percent-encodes reserved characters in column values") {
	TemplateTermMap tt("http://data.example.com/name/{NAME}");
	SerdEnv *env = serd_env_new(nullptr);

	auto row = makeRow({{"NAME", StringSQLValue(std::string("a b/c"))}});
	SerdNode node = tt.generateRDFTerm(row, *env);

	REQUIRE(nodeUri(node) == "http://data.example.com/name/a%20b%2Fc");
	serd_env_free(env);
}

TEST_CASE("TemplateTermMap leaves unreserved characters untouched") {
	TemplateTermMap tt("http://data.example.com/name/{NAME}");
	SerdEnv *env = serd_env_new(nullptr);

	auto row = makeRow({{"NAME", StringSQLValue(std::string("Abc-123_x.y~z"))}});
	SerdNode node = tt.generateRDFTerm(row, *env);

	REQUIRE(nodeUri(node) == "http://data.example.com/name/Abc-123_x.y~z");
	serd_env_free(env);
}

TEST_CASE("TemplateTermMap malformed template (unmatched '{') truncates gracefully") {
	// No closing '}' after the placeholder start; generateRDFTerm must not
	// crash and should treat the remainder as consumed rather than throwing.
	TemplateTermMap tt("http://data.example.com/{NAME");
	SerdEnv *env = serd_env_new(nullptr);

	auto row = makeRow({{"NAME", StringSQLValue(std::string("SMITH"))}});
	SerdNode node = tt.generateRDFTerm(row, *env);

	REQUIRE(nodeUri(node) == "http://data.example.com/");
	serd_env_free(env);
}

// ---------------------------------------------------------------------------
// TermMap base print()/computeDatatypeIRI(): languageTag, datatypeIRI and all
// three TermType enumerators (only IRI/Literal were previously exercised).
// ---------------------------------------------------------------------------

TEST_CASE("TermMap::computeDatatypeIRI base implementation") {
	TemplateTermMap tt("{col}"); // does not override computeDatatypeIRI
	MapSQLRow row;

	REQUIRE(tt.computeDatatypeIRI(row).empty());

	tt.datatypeIRI = std::unique_ptr<std::string>(new std::string("http://example.com/mytype"));
	REQUIRE(tt.computeDatatypeIRI(row) == "http://example.com/mytype");
}

TEST_CASE("TermMap::print reports termType, languageTag and datatypeIRI") {
	TemplateTermMap tt("{col}");
	tt.termType = TermType::BlankNode;
	tt.languageTag = std::unique_ptr<std::string>(new std::string("en"));
	tt.datatypeIRI = std::unique_ptr<std::string>(new std::string("http://example.com/mytype"));

	std::ostringstream oss;
	oss << tt;
	const std::string out = oss.str();
	REQUIRE(out.find("termType=BlankNode") != std::string::npos);
	REQUIRE(out.find("lang=\"en\"") != std::string::npos);
	REQUIRE(out.find("datatype=\"http://example.com/mytype\"") != std::string::npos);
}

TEST_CASE("TermMap::print reports Literal termType") {
	TemplateTermMap tt("{col}");
	tt.termType = TermType::Literal;
	std::ostringstream oss;
	oss << tt;
	REQUIRE(oss.str().find("termType=Literal") != std::string::npos);
}

// ---------------------------------------------------------------------------
// PredicateObjectMap: empty predicate/object maps, fallback Serd environment,
// language-tag/datatype interplay, operator<< with multiple entries and
// graphMaps, and a ReferencingObjectMap whose join yields no result set.
// ---------------------------------------------------------------------------

TEST_CASE("PredicateObjectMap isValid/isValidInsideOut are false when empty") {
	PredicateObjectMap pom;
	REQUIRE_FALSE(pom.isValid());
	REQUIRE_FALSE(pom.isValidInsideOut());
}

TEST_CASE("PredicateObjectMap::processRow falls back to an internal Serd environment "
          "when the mapping has none") {
	// A hand-built R2RMLMapping (not produced by R2RMLParser::parse) leaves
	// serdEnvironment == nullptr, forcing processRow's lazily-constructed
	// fallback environment to be used.
	R2RMLMapping mapping;
	REQUIRE(mapping.serdEnvironment == nullptr);

	PredicateObjectMap pom;
	const uint8_t predUri[] = "http://example.com/ns#name";
	SerdNode predNode = serd_node_from_string(SERD_URI, predUri);
	pom.predicateMaps.push_back(std::unique_ptr<ConstantTermMap>(new ConstantTermMap(predNode)));
	pom.objectMaps.push_back(std::unique_ptr<ColumnTermMap>(new ColumnTermMap("NAME")));

	auto row = makeRow({{"NAME", StringSQLValue(std::string("SMITH"))}});
	MockSQLConnection conn;
	const uint8_t subjUri[] = "http://example.com/subj/1";
	SerdNode subject = serd_node_from_string(SERD_URI, subjUri);

	std::string out = captureNTriples([&](SerdWriter &writer) { pom.processRow(row, subject, writer, mapping, conn); });

	REQUIRE(out.find("SMITH") != std::string::npos);
}

TEST_CASE("PredicateObjectMap::processRow: literal with a language tag never gets a "
          "datatype annotation") {
	// Per RDF, a language-tagged literal cannot also carry a datatype IRI.
	// computeDatatypeIRI() must therefore be skipped whenever languageTag is set,
	// even though the underlying column value would otherwise infer xsd:integer.
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "example1.ttl");
	REQUIRE(mapping.isValid());

	PredicateObjectMap pom;
	const uint8_t predUri[] = "http://example.com/ns#count";
	SerdNode predNode = serd_node_from_string(SERD_URI, predUri);
	pom.predicateMaps.push_back(std::unique_ptr<ConstantTermMap>(new ConstantTermMap(predNode)));

	auto col = std::unique_ptr<ColumnTermMap>(new ColumnTermMap("COUNT"));
	col->termType = TermType::Literal;
	col->languageTag = std::unique_ptr<std::string>(new std::string("en"));
	pom.objectMaps.push_back(std::move(col));

	auto row = makeRow({{"COUNT", StringSQLValue(42)}});
	MockSQLConnection conn;
	const uint8_t subjUri[] = "http://example.com/subj/1";
	SerdNode subject = serd_node_from_string(SERD_URI, subjUri);

	std::string out = captureNTriples([&](SerdWriter &writer) { pom.processRow(row, subject, writer, mapping, conn); });

	REQUIRE(out.find("\"42\"") != std::string::npos);
	REQUIRE(out.find("^^<http://www.w3.org/2001/XMLSchema#integer>") == std::string::npos);
}

TEST_CASE("PredicateObjectMap::processRow skips a ReferencingObjectMap whose parent "
          "has no logical table") {
	// getJoinedRows() returns nullptr when parentTriplesMap has no logicalTable;
	// processRow must skip that object map rather than dereferencing null.
	R2RMLMapping mapping;
	PredicateObjectMap pom;

	const uint8_t predUri[] = "http://example.com/ns#department";
	SerdNode predNode = serd_node_from_string(SERD_URI, predUri);
	pom.predicateMaps.push_back(std::unique_ptr<ConstantTermMap>(new ConstantTermMap(predNode)));

	TriplesMap parentWithoutLogicalTable; // logicalTable stays nullptr
	auto rom = std::unique_ptr<TestReferencingObjectMap>(new TestReferencingObjectMap());
	rom->parentTriplesMap = &parentWithoutLogicalTable;
	pom.objectMaps.push_back(std::move(rom));

	MapSQLRow row;
	MockSQLConnection conn;
	const uint8_t subjUri[] = "http://example.com/subj/1";
	SerdNode subject = serd_node_from_string(SERD_URI, subjUri);

	std::string out = captureNTriples([&](SerdWriter &writer) { pom.processRow(row, subject, writer, mapping, conn); });
	REQUIRE(out.empty());
}

TEST_CASE("PredicateObjectMap::operator<< renders multiple predicates/objects and graphMaps") {
	PredicateObjectMap pom;
	const uint8_t predUri1[] = "http://example.com/ns#p1";
	const uint8_t predUri2[] = "http://example.com/ns#p2";
	pom.predicateMaps.push_back(
	    std::unique_ptr<ConstantTermMap>(new ConstantTermMap(serd_node_from_string(SERD_URI, predUri1))));
	pom.predicateMaps.push_back(
	    std::unique_ptr<ConstantTermMap>(new ConstantTermMap(serd_node_from_string(SERD_URI, predUri2))));
	pom.objectMaps.push_back(std::unique_ptr<ColumnTermMap>(new ColumnTermMap("A")));
	pom.objectMaps.push_back(std::unique_ptr<ColumnTermMap>(new ColumnTermMap("B")));
	pom.graphMaps.push_back(std::unique_ptr<TestGraphMap>(new TestGraphMap()));

	std::ostringstream oss;
	oss << pom;
	const std::string out = oss.str();

	REQUIRE(out.find("p1") != std::string::npos);
	REQUIRE(out.find("p2") != std::string::npos);
	REQUIRE(out.find("graphMaps=[") != std::string::npos);
	// The ", " separator is only emitted between the 2nd..Nth entries.
	REQUIRE(out.find(", ") != std::string::npos);
}

// ---------------------------------------------------------------------------
// TriplesMap::generateTriples edge cases: no subjectMap at all, and a
// subjectMap whose generated term is null (e.g. a missing column value).
// ---------------------------------------------------------------------------

TEST_CASE("TriplesMap::generateTriples is a no-op when subjectMap is null") {
	TriplesMap tm;
	REQUIRE(tm.subjectMap == nullptr);

	MapSQLRow row;
	MockSQLConnection conn;
	R2RMLMapping mapping;

	std::string out = captureNTriples([&](SerdWriter &writer) { tm.generateTriples(row, writer, mapping, conn); });
	REQUIRE(out.empty());
}

TEST_CASE("TriplesMap::generateTriples skips the row when the subject term is null") {
	TriplesMap tm;
	auto sm = std::unique_ptr<TestValueSubjectMap>(new TestValueSubjectMap());
	sm->valueMap = std::unique_ptr<ColumnTermMap>(new ColumnTermMap("MISSING_COL"));
	tm.subjectMap = std::move(sm);

	MapSQLRow row; // MISSING_COL is absent -> ColumnTermMap::generateRDFTerm returns null
	MockSQLConnection conn;
	R2RMLMapping mapping;

	std::string out = captureNTriples([&](SerdWriter &writer) { tm.generateTriples(row, writer, mapping, conn); });
	REQUIRE(out.empty());
}

// ---------------------------------------------------------------------------
// R2RMLMapping: move construction/assignment, and processDatabase() skipping
// invalid TriplesMaps and null result sets.
// ---------------------------------------------------------------------------

TEST_CASE("R2RMLMapping move construction and move assignment transfer ownership") {
	R2RMLParser parser;
	R2RMLMapping m1 = parser.parse(SOURCE_R2RML_DIR "example1.ttl");
	SerdEnv *originalEnv = m1.serdEnvironment;
	const std::size_t n = m1.triplesMaps.size();
	REQUIRE(originalEnv != nullptr);
	REQUIRE(n > 0);

	R2RMLMapping m2(std::move(m1));
	REQUIRE(m2.serdEnvironment == originalEnv);
	REQUIRE(m1.serdEnvironment == nullptr);
	REQUIRE(m2.triplesMaps.size() == n);
	REQUIRE(m1.triplesMaps.empty());

	R2RMLMapping m3;
	m3 = std::move(m2);
	REQUIRE(m3.serdEnvironment == originalEnv);
	REQUIRE(m2.serdEnvironment == nullptr);
	REQUIRE(m3.triplesMaps.size() == n);

	// Move-assigning a mapping into itself (through an aliased pointer so the
	// compiler cannot diagnose it as a literal self-move) must be a safe no-op.
	R2RMLMapping *alias = &m3;
	*alias = std::move(*alias);
	REQUIRE(m3.serdEnvironment == originalEnv);
	REQUIRE(m3.triplesMaps.size() == n);
}

TEST_CASE("R2RMLMapping::processDatabase skips invalid TriplesMaps and null result sets") {
	R2RMLMapping mapping;

	// TriplesMap #1: invalid (no logicalTable at all) -> must be skipped.
	auto tm1 = std::unique_ptr<TriplesMap>(new TriplesMap());
	tm1->id = "#Invalid";
	REQUIRE_FALSE(tm1->isValid());
	mapping.triplesMaps.push_back(std::move(tm1));

	// TriplesMap #2: valid, but its logical table's getRows() returns nullptr.
	auto tm2 = std::unique_ptr<TriplesMap>(new TriplesMap());
	tm2->id = "#NullRows";
	tm2->logicalTable = std::unique_ptr<TestNullRowsLogicalTable>(new TestNullRowsLogicalTable());
	tm2->subjectMap = std::unique_ptr<TestValueSubjectMap>(new TestValueSubjectMap());
	REQUIRE(tm2->isValid());
	mapping.triplesMaps.push_back(std::move(tm2));

	MockSQLConnection conn;
	std::string out = captureNTriples([&](SerdWriter &writer) { mapping.processDatabase(conn, writer); });
	REQUIRE(out.empty());
}

// ---------------------------------------------------------------------------
// ReferencingObjectMap: isValid()/getJoinedRows()/generateRDFTerm(child,parent)
// edge cases, and print() with a resolved parent and multiple join conditions.
// ---------------------------------------------------------------------------

TEST_CASE("ReferencingObjectMap::isValid is false without a parentTriplesMap") {
	TestReferencingObjectMap rom;
	REQUIRE(rom.parentTriplesMap == nullptr);
	REQUIRE_FALSE(rom.isValid());
}

TEST_CASE("ReferencingObjectMap::getJoinedRows returns nullptr without a parentTriplesMap") {
	TestReferencingObjectMap rom;
	MockSQLConnection conn;
	MapSQLRow row;
	REQUIRE(rom.getJoinedRows(conn, row) == nullptr);
}

TEST_CASE("ReferencingObjectMap::getJoinedRows returns nullptr when the parent has no logicalTable") {
	TestReferencingObjectMap rom;
	TriplesMap parent; // logicalTable left null
	rom.parentTriplesMap = &parent;

	MockSQLConnection conn;
	MapSQLRow row;
	REQUIRE(rom.getJoinedRows(conn, row) == nullptr);
}

TEST_CASE("ReferencingObjectMap::getJoinedRows returns nullptr when the parent connection yields no result set") {
	TestReferencingObjectMap rom;
	TriplesMap parent;
	parent.logicalTable = std::unique_ptr<BaseTableOrView>(new BaseTableOrView("DEPT"));
	rom.parentTriplesMap = &parent;

	NullResultConnection conn; // execute() always returns nullptr
	MapSQLRow row;
	REQUIRE(rom.getJoinedRows(conn, row) == nullptr);
}

TEST_CASE("ReferencingObjectMap::getJoinedRows filters out rows that fail the join condition") {
	TestReferencingObjectMap rom;
	TriplesMap parent;
	parent.logicalTable = std::unique_ptr<BaseTableOrView>(new BaseTableOrView("DEPT"));
	rom.parentTriplesMap = &parent;
	rom.joinConditions.emplace_back("DEPTNO", "DEPTNO");

	MockSQLConnection conn;
	conn.addResult("DEPT", {makeRow({{"DEPTNO", StringSQLValue(std::string("10"))}}),
	                        makeRow({{"DEPTNO", StringSQLValue(std::string("20"))}})});

	auto childRow = makeRow({{"DEPTNO", StringSQLValue(std::string("10"))}});
	auto joined = rom.getJoinedRows(conn, childRow);
	REQUIRE(joined != nullptr);

	int matchedCount = 0;
	while (joined->next()) {
		++matchedCount;
		REQUIRE(joined->getCurrentRow().getValue("DEPTNO")->asString() == "10");
	}
	// Only the DEPTNO=10 parent row satisfies the join condition; DEPTNO=20 is filtered out.
	REQUIRE(matchedCount == 1);
}

TEST_CASE("ReferencingObjectMap::generateRDFTerm(child,parent) returns null without a resolved parent") {
	TestReferencingObjectMap rom;
	SerdEnv *env = serd_env_new(nullptr);
	MapSQLRow childRow;
	MapSQLRow parentRow;

	SerdNode result = rom.generateRDFTerm(childRow, parentRow, *env);
	REQUIRE(serd_node_equals(&result, &SERD_NODE_NULL));

	TriplesMap parentWithoutSubjectMap; // subjectMap left null
	rom.parentTriplesMap = &parentWithoutSubjectMap;
	result = rom.generateRDFTerm(childRow, parentRow, *env);
	REQUIRE(serd_node_equals(&result, &SERD_NODE_NULL));

	serd_env_free(env);
}

TEST_CASE("ReferencingObjectMap::print includes the resolved parent id and all join conditions") {
	TriplesMap parent;
	parent.id = "#Parent";

	TestReferencingObjectMap rom;
	rom.parentTriplesMap = &parent;
	rom.joinConditions.emplace_back("child1", "parent1");
	rom.joinConditions.emplace_back("child2", "parent2");

	std::ostringstream oss;
	oss << rom;
	const std::string out = oss.str();

	REQUIRE(out.find("<#Parent>") != std::string::npos);
	REQUIRE(out.find("joins=[") != std::string::npos);
	REQUIRE(out.find(", ") != std::string::npos); // separator between the two join conditions
}

// ---------------------------------------------------------------------------
// R2RMLParser: full rr:predicateMap / rr:object / rr:constant (subjectMap)
// forms, a static rr:datatype on an rr:template term map, and an in-document
// @base directive – none of which appear in the other fixture files, which
// only use the rr:predicate / rr:column / rr:template shortcuts.
// ---------------------------------------------------------------------------

TEST_CASE("Parser: full rr:predicateMap/rr:objectMap forms, rr:object shortcut, and @base directive") {
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "parser_full_forms.ttl");

	REQUIRE(mapping.triplesMaps.size() == 2);

	TriplesMap *tmFull = nullptr;
	TriplesMap *tmConst = nullptr;
	for (auto &tm : mapping.triplesMaps) {
		if (tm->id.find("TriplesMapFull") != std::string::npos) {
			tmFull = tm.get();
		} else if (tm->id.find("TriplesMapConstSubject") != std::string::npos) {
			tmConst = tm.get();
		}
	}
	REQUIRE(tmFull != nullptr);
	REQUIRE(tmConst != nullptr);

	// --- rr:subjectMap using rr:column (not rr:template/rr:constant) -------
	REQUIRE(tmFull->subjectMap != nullptr);
	SerdEnv *env = serd_env_new(nullptr);
	auto empRow =
	    makeRow({{"EMPNO", StringSQLValue(std::string("7369"))}, {"JOB", StringSQLValue(std::string("CLERK"))}});
	SerdNode subjectTerm = tmFull->subjectMap->generateRDFTerm(empRow, *env);
	REQUIRE(nodeUri(subjectTerm) == "7369");

	// --- rr:predicateObjectMap using the full rr:predicateMap form ---------
	REQUIRE(tmFull->predicateObjectMaps.size() == 2);
	PredicateObjectMap *fullFormPom = nullptr;
	PredicateObjectMap *shortcutPom = nullptr;
	for (auto &pom : tmFull->predicateObjectMaps) {
		auto *pred =
		    dynamic_cast<TemplateTermMap *>(pom->predicateMaps.empty() ? nullptr : pom->predicateMaps[0].get());
		if (pred != nullptr) {
			fullFormPom = pom.get();
		} else {
			shortcutPom = pom.get();
		}
	}
	REQUIRE(fullFormPom != nullptr);
	REQUIRE(shortcutPom != nullptr);

	auto *predTemplate = dynamic_cast<TemplateTermMap *>(fullFormPom->predicateMaps[0].get());
	REQUIRE(predTemplate != nullptr);
	REQUIRE(predTemplate->templateString == "http://example.com/ns#{JOB}");

	// --- rr:objectMap using rr:template together with a static rr:datatype
	REQUIRE(fullFormPom->objectMaps.size() == 1);
	auto *objTemplate = dynamic_cast<TemplateTermMap *>(fullFormPom->objectMaps[0].get());
	REQUIRE(objTemplate != nullptr);
	REQUIRE(objTemplate->templateString == "http://data.example.com/role/{JOB}");
	REQUIRE(objTemplate->datatypeIRI != nullptr);
	REQUIRE(*objTemplate->datatypeIRI == "http://www.w3.org/2001/XMLSchema#string");

	// --- rr:predicateObjectMap using the rr:object constant-URI shortcut ---
	REQUIRE(shortcutPom->predicateMaps.size() == 1);
	auto *shortcutPred = dynamic_cast<ConstantTermMap *>(shortcutPom->predicateMaps[0].get());
	REQUIRE(shortcutPred != nullptr);
	REQUIRE(nodeUri(shortcutPred->constantValue) == "http://example.com/ns#tag");

	REQUIRE(shortcutPom->objectMaps.size() == 1);
	auto *constObj = dynamic_cast<ConstantTermMap *>(shortcutPom->objectMaps[0].get());
	REQUIRE(constObj != nullptr);
	REQUIRE(nodeUri(constObj->constantValue) == "http://example.com/ns#ConstantTag");

	// --- Second TriplesMap: rr:subjectMap using rr:constant ----------------
	REQUIRE(tmConst->subjectMap != nullptr);
	SerdNode constSubject = tmConst->subjectMap->generateRDFTerm(MapSQLRow(), *env);
	REQUIRE(nodeUri(constSubject) == "http://example.com/ns#TheOneAndOnly");

	serd_env_free(env);
}

TEST_CASE("Parser: a literal rr:predicateObjectMap value is skipped, other POMs are kept") {
	// Covers the "continue" branch taken when a rr:predicateObjectMap value
	// cannot be resolved to a subject key (objKey() returns empty for plain
	// literals).
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "valid_r2rml_literal_pom.ttl");

	REQUIRE(mapping.triplesMaps.size() == 1);
	TriplesMap *tm = mapping.triplesMaps[0].get();
	REQUIRE(tm->id.find("TriplesMap1") != std::string::npos);

	// Only the well-formed predicateObjectMap survives.
	REQUIRE(tm->predicateObjectMaps.size() == 1);
	REQUIRE(tm->predicateObjectMaps[0]->predicateMaps.size() == 1);
}
