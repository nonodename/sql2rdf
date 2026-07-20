/**
 * Regression tests: parsing a mapping file via a *relative* filesystem path
 * must still produce an absolute "file://" document base URI.
 *
 * serd_node_new_file_uri() only recognises a path as absolute (and therefore
 * worth a "file://" scheme) if it already starts with a directory separator
 * (or drive letter on Windows); a relative path like "mapping.ttl" is
 * returned percent-escaped but schemeless. Any relative reference inside the
 * mapping that is resolved against that broken base (e.g. a plain
 * `rr:predicate <#name>`, or a YARRRML po predicate that isn't an absolute
 * IRI/CURIE) then stays schemeless too, and later fails to write as
 * N-Triples/N-Quads. r2rml::toAbsoluteFilePath() (used by both
 * R2RMLParser::parse() and YARRRMLParser::parse()) fixes this by resolving
 * relative mapping paths against the current working directory first.
 */

#include <catch2/catch_test_macros.hpp>

#include <serd/serd.h>

#include <string>

// Fallback for IDE tooling; CMake overrides via target_compile_definitions.
#ifndef SOURCE_R2RML_DIR
#define SOURCE_R2RML_DIR ""
#endif
#ifndef SOURCE_YARRRML_DIR
#define SOURCE_YARRRML_DIR ""
#endif

#include "r2rml/ConstantTermMap.h"
#include "r2rml/PredicateObjectMap.h"
#include "r2rml/R2RMLMapping.h"
#include "r2rml/R2RMLParser.h"
#include "r2rml/TriplesMap.h"
#include "yarrrml/YARRRMLParser.h"

#ifdef _WIN32
#include <direct.h>
#define R2RDF_TEST_GETCWD _getcwd
#define R2RDF_TEST_CHDIR _chdir
#else
#include <unistd.h>
#define R2RDF_TEST_GETCWD getcwd
#define R2RDF_TEST_CHDIR chdir
#endif

using r2rml::ConstantTermMap;
using r2rml::R2RMLMapping;
using r2rml::R2RMLParser;
using yarrrml::YARRRMLParser;

namespace {

// Temporarily changes the process's working directory to `dir`, restoring
// the original directory on destruction (including when a REQUIRE
// assertion fails and unwinds out of the test case early).
class ScopedWorkingDirectory {
public:
	explicit ScopedWorkingDirectory(const std::string &dir) {
		char buffer[4096];
		if (R2RDF_TEST_GETCWD(buffer, sizeof(buffer))) {
			original_ = buffer;
		}
		R2RDF_TEST_CHDIR(dir.c_str());
	}

	~ScopedWorkingDirectory() {
		if (!original_.empty()) {
			R2RDF_TEST_CHDIR(original_.c_str());
		}
	}

	ScopedWorkingDirectory(const ScopedWorkingDirectory &) = delete;
	ScopedWorkingDirectory &operator=(const ScopedWorkingDirectory &) = delete;

private:
	std::string original_;
};

// Returns the resolved IRI stored by the mapping's sole TriplesMap / sole
// predicateObjectMap / sole predicate ConstantTermMap.
std::string firstPredicateIri(const R2RMLMapping &mapping) {
	REQUIRE(mapping.triplesMaps.size() == 1);
	const auto &tm = mapping.triplesMaps[0];
	REQUIRE(tm->predicateObjectMaps.size() == 1);
	const auto &pom = tm->predicateObjectMaps[0];
	REQUIRE(pom->predicateMaps.size() == 1);
	auto *predMap = dynamic_cast<ConstantTermMap *>(pom->predicateMaps[0].get());
	REQUIRE(predMap != nullptr);
	const SerdNode &node = predMap->constantValue;
	REQUIRE(node.buf != nullptr);
	return std::string(reinterpret_cast<const char *>(node.buf), node.n_bytes);
}

bool endsWith(const std::string &value, const std::string &suffix) {
	return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

TEST_CASE("R2RMLParser::parse resolves a relative mapping path to an absolute file base URI") {
	ScopedWorkingDirectory cwd(SOURCE_R2RML_DIR);

	R2RMLParser parser;
	R2RMLMapping mapping;
	REQUIRE_NOTHROW(mapping = parser.parse("relative_base_predicate.ttl"));

	std::string predicateIri = firstPredicateIri(mapping);
	// Before the fix this stays schemeless (e.g.
	// "relative_base_predicate.ttl#name"); the fix makes it absolute.
	REQUIRE(predicateIri.compare(0, 7, "file://") == 0);
	REQUIRE(endsWith(predicateIri, "#name"));
}

TEST_CASE("YARRRMLParser::parse resolves a relative mapping path to an absolute file base URI") {
	ScopedWorkingDirectory cwd(SOURCE_YARRRML_DIR);

	YARRRMLParser parser;
	R2RMLMapping mapping;
	REQUIRE_NOTHROW(mapping = parser.parse("relative_base_predicate.yml"));

	std::string predicateIri = firstPredicateIri(mapping);
	REQUIRE(predicateIri.compare(0, 7, "file://") == 0);
	REQUIRE(endsWith(predicateIri, "#name"));
}
