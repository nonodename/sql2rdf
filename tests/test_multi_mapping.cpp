#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <vector>

// Fallback for IDE tooling; CMake overrides this via target_compile_definitions.
#ifndef SOURCE_R2RML_DIR
#define SOURCE_R2RML_DIR ""
#endif
#ifndef SOURCE_YARRRML_DIR
#define SOURCE_YARRRML_DIR ""
#endif

#include "r2rml/BaseTableOrView.h"
#include "r2rml/LogicalTable.h"
#include "r2rml/MappingParser.h"
#include "r2rml/PredicateObjectMap.h"
#include "r2rml/R2RMLMapping.h"
#include "r2rml/R2RMLParser.h"
#include "r2rml/ReferencingObjectMap.h"
#include "r2rml/SubjectMap.h"
#include "r2rml/TemplateTermMap.h"
#include "r2rml/TriplesMap.h"
#include "yarrrml/YARRRMLParser.h"

using r2rml::BaseTableOrView;
using r2rml::MappingParser;
using r2rml::R2RMLMapping;
using r2rml::R2RMLParser;
using r2rml::ReferencingObjectMap;
using r2rml::TemplateTermMap;
using r2rml::TriplesMap;
using yarrrml::YARRRMLParser;

namespace {

TriplesMap *findById(R2RMLMapping &m, const std::string &fragment) {
	for (auto &tm : m.triplesMaps) {
		if (tm->id.find(fragment) != std::string::npos) {
			return tm.get();
		}
	}
	return nullptr;
}

std::string tableNameOf(TriplesMap &tm) {
	auto *table = dynamic_cast<BaseTableOrView *>(tm.logicalTable.get());
	REQUIRE(table != nullptr);
	return table->tableName;
}

} // namespace

TEST_CASE("parseMultiple merges non-conflicting R2RML files with no warnings") {
	R2RMLMapping mapping =
	    MappingParser::parseMultiple({SOURCE_R2RML_DIR "multi_dept.ttl", SOURCE_R2RML_DIR "multi_blank_a.ttl"});

	REQUIRE(mapping.triplesMaps.size() == 2);
	REQUIRE(findById(mapping, "DeptTM") != nullptr);
	REQUIRE(findById(mapping, "BlankTMA") != nullptr);
	REQUIRE(mapping.mergeWarnings.empty());
}

TEST_CASE("parseMultiple resolves rr:parentTriplesMap across files") {
	R2RMLMapping mapping =
	    MappingParser::parseMultiple({SOURCE_R2RML_DIR "multi_dept.ttl", SOURCE_R2RML_DIR "multi_emp.ttl"});

	REQUIRE(mapping.triplesMaps.size() == 2);
	TriplesMap *emp = findById(mapping, "EmpTM");
	TriplesMap *dept = findById(mapping, "DeptTM");
	REQUIRE(emp != nullptr);
	REQUIRE(dept != nullptr);

	REQUIRE(emp->predicateObjectMaps.size() == 1);
	REQUIRE(emp->predicateObjectMaps[0]->objectMaps.size() == 1);
	auto *rom = dynamic_cast<ReferencingObjectMap *>(emp->predicateObjectMaps[0]->objectMaps[0].get());
	REQUIRE(rom != nullptr);
	REQUIRE(rom->parentTriplesMap == dept);
	REQUIRE(mapping.mergeWarnings.empty());
}

TEST_CASE("Loading the referencing file alone still yields an unresolved parentTriplesMap error") {
	// Regression guard: without multi_dept.ttl in the mix, ex:DeptTM is simply
	// undefined and today's existing single-file resolution behaviour must be
	// unchanged.
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "multi_emp.ttl");

	REQUIRE(mapping.triplesMaps.size() == 1);
	bool foundUnresolved = false;
	for (const auto &err : mapping.parseErrors) {
		if (err.find("unresolved parentTriplesMap") != std::string::npos && err.find("DeptTM") != std::string::npos) {
			foundUnresolved = true;
		}
	}
	REQUIRE(foundUnresolved);
}

TEST_CASE("parseMultiple keeps the first file's definition on conflict and warns") {
	R2RMLMapping mapping = MappingParser::parseMultiple(
	    {SOURCE_R2RML_DIR "multi_conflict_a.ttl", SOURCE_R2RML_DIR "multi_conflict_b.ttl"});

	REQUIRE(mapping.triplesMaps.size() == 1);
	TriplesMap *tm = findById(mapping, "ConflictTM");
	REQUIRE(tm != nullptr);
	REQUIRE(tableNameOf(*tm) == "TABLE_A");

	REQUIRE(mapping.mergeWarnings.size() == 1);
	REQUIRE(mapping.mergeWarnings[0].find("ConflictTM") != std::string::npos);
	REQUIRE(mapping.mergeWarnings[0].find("multi_conflict_b.ttl") != std::string::npos);
	REQUIRE(mapping.mergeWarnings[0].find("multi_conflict_a.ttl") != std::string::npos);
}

TEST_CASE("parseMultiple conflict resolution is order-driven, not IRI-driven") {
	R2RMLMapping mapping = MappingParser::parseMultiple(
	    {SOURCE_R2RML_DIR "multi_conflict_b.ttl", SOURCE_R2RML_DIR "multi_conflict_a.ttl"});

	REQUIRE(mapping.triplesMaps.size() == 1);
	TriplesMap *tm = findById(mapping, "ConflictTM");
	REQUIRE(tm != nullptr);
	REQUIRE(tableNameOf(*tm) == "TABLE_B");
	REQUIRE(mapping.mergeWarnings.size() == 1);
}

TEST_CASE("parseMultiple conflict warnings are emitted even in strict mode") {
	R2RMLMapping mapping =
	    MappingParser::parseMultiple({SOURCE_R2RML_DIR "multi_conflict_a.ttl", SOURCE_R2RML_DIR "multi_conflict_b.ttl"},
	                                 /*ignoreNonFatalErrors=*/false);

	REQUIRE(mapping.triplesMaps.size() == 1);
	REQUIRE(mapping.mergeWarnings.size() == 1);
	REQUIRE(mapping.parseErrors.empty());
}

TEST_CASE("parseMultiple scopes blank node labels per source file") {
	// Both files' first (and only) blank node is an anonymous inline
	// logicalTable/subjectMap - independently authored files very commonly
	// mint blank labels from the same starting point, so without per-source
	// scoping this merge would corrupt one or both TriplesMaps.
	R2RMLMapping mapping =
	    MappingParser::parseMultiple({SOURCE_R2RML_DIR "multi_blank_a.ttl", SOURCE_R2RML_DIR "multi_blank_b.ttl"});

	REQUIRE(mapping.triplesMaps.size() == 2);
	TriplesMap *a = findById(mapping, "BlankTMA");
	TriplesMap *b = findById(mapping, "BlankTMB");
	REQUIRE(a != nullptr);
	REQUIRE(b != nullptr);

	REQUIRE(tableNameOf(*a) == "TABLE_BLANK_A");
	REQUIRE(tableNameOf(*b) == "TABLE_BLANK_B");

	auto *aTemplate = dynamic_cast<const TemplateTermMap *>(a->subjectMap->valueTermMap());
	auto *bTemplate = dynamic_cast<const TemplateTermMap *>(b->subjectMap->valueTermMap());
	REQUIRE(aTemplate != nullptr);
	REQUIRE(bTemplate != nullptr);
	REQUIRE(aTemplate->templateString == "http://data.example.com/blank-a/{ID}");
	REQUIRE(bTemplate->templateString == "http://data.example.com/blank-b/{ID}");
}

TEST_CASE("parseMultiple merges mixed R2RML and YARRRML files") {
	R2RMLMapping mapping =
	    MappingParser::parseMultiple({SOURCE_R2RML_DIR "multi_dept.ttl", SOURCE_YARRRML_DIR "multi_extra.yml"});

	REQUIRE(mapping.triplesMaps.size() == 2);
	TriplesMap *dept = findById(mapping, "DeptTM");
	TriplesMap *extra = findById(mapping, "ExtraTM");
	REQUIRE(dept != nullptr);
	REQUIRE(extra != nullptr);
	REQUIRE(tableNameOf(*dept) == "DEPT");
	REQUIRE(tableNameOf(*extra) == "EXTRA");
	REQUIRE(mapping.mergeWarnings.empty());
}

TEST_CASE("parseMultiple conflict detection is format-agnostic") {
	R2RMLMapping ttlFirst = MappingParser::parseMultiple(
	    {SOURCE_R2RML_DIR "multi_conflict_a.ttl", SOURCE_YARRRML_DIR "multi_conflict_c.yml"});
	REQUIRE(ttlFirst.triplesMaps.size() == 1);
	REQUIRE(tableNameOf(*findById(ttlFirst, "ConflictTM")) == "TABLE_A");
	REQUIRE(ttlFirst.mergeWarnings.size() == 1);

	R2RMLMapping yamlFirst = MappingParser::parseMultiple(
	    {SOURCE_YARRRML_DIR "multi_conflict_c.yml", SOURCE_R2RML_DIR "multi_conflict_a.ttl"});
	REQUIRE(yamlFirst.triplesMaps.size() == 1);
	REQUIRE(tableNameOf(*findById(yamlFirst, "ConflictTM")) == "TABLE_C");
	REQUIRE(yamlFirst.mergeWarnings.size() == 1);
}

TEST_CASE("parseMultiple with a single file matches the single-file parse() result") {
	R2RMLMapping viaMultiple = MappingParser::parseMultiple({SOURCE_R2RML_DIR "example1.ttl"});

	R2RMLParser parser;
	R2RMLMapping viaSingle = parser.parse(SOURCE_R2RML_DIR "example1.ttl");

	REQUIRE(viaMultiple.triplesMaps.size() == viaSingle.triplesMaps.size());
	REQUIRE(viaMultiple.triplesMaps.size() == 1);
	REQUIRE(viaMultiple.triplesMaps[0]->id == viaSingle.triplesMaps[0]->id);
	REQUIRE(viaMultiple.parseErrors.empty());
	REQUIRE(viaMultiple.mergeWarnings.empty());
}

TEST_CASE("parseMultiple forceYarrrml forces every file to be parsed as YARRRML regardless of extension") {
	// multi_yarrrml_content.ttl is deliberately YARRRML YAML saved under a
	// .ttl extension: without forceYarrrml it's dispatched (wrongly) to the
	// Turtle parser, which can't make sense of it and yields no TriplesMap;
	// with forceYarrrml it's correctly translated.
	R2RMLMapping withoutForce = MappingParser::parseMultiple({SOURCE_R2RML_DIR "multi_yarrrml_content.ttl"});
	REQUIRE(withoutForce.triplesMaps.empty());

	R2RMLMapping withForce = MappingParser::parseMultiple({SOURCE_R2RML_DIR "multi_yarrrml_content.ttl"},
	                                                      /*ignoreNonFatalErrors=*/true, /*forceYarrrml=*/true);
	REQUIRE(withForce.triplesMaps.size() == 1);
	REQUIRE(findById(withForce, "ForcedTM") != nullptr);
}

TEST_CASE("parseMultiple rejects an unrecognised file extension") {
	REQUIRE_THROWS_AS(MappingParser::parseMultiple({SOURCE_R2RML_DIR "example1.ttl", "no-such-extension.foo"}),
	                  std::runtime_error);
}
