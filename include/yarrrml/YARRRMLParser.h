#pragma once

#include <string>

#include "r2rml/MappingParser.h"

namespace r2rml {
class R2RMLMapping;
} // namespace r2rml

namespace yarrrml {

/**
 * Translates YARRRML (https://rml.io/yarrrml/spec/) YAML mapping documents
 * into the r2rml object model.
 *
 * Internally this translates the supported YARRRML subset directly into
 * RDF statements fed to an r2rml::TripleCollector and reuses
 * r2rml::R2RMLParser::parseCollected() to build the object model, so all
 * downstream engine behaviour (term-map generation, joins, datatypes, etc.)
 * is identical between R2RML and YARRRML mappings.
 */
class YARRRMLParser : public r2rml::MappingParser {
public:
	YARRRMLParser() = default;
	~YARRRMLParser() override = default;

	/// Return true if `path`'s extension indicates a YARRRML (YAML) mapping file.
	static bool hasYarrrmlExtension(const std::string &path);

	/**
	 * Read `yarrrmlFilePath` and translate it into the R2RML object model.
	 *
	 * @param ignoreNonFatalErrors
	 *   - true  (default): non-fatal issues found either during YARRRML
	 *     translation (unsupported keys, skipped graphs, multiple sources,
	 *     unresolved condition functions, ...) or during the underlying
	 *     R2RML build are collected into the returned mapping's parseErrors.
	 *   - false: any such non-fatal issue causes a std::runtime_error to be
	 *     thrown instead.
	 *
	 * Fatal problems (unreadable file, YAML syntax error, missing `mappings`
	 * key) always throw std::runtime_error, regardless of this flag.
	 */
	r2rml::R2RMLMapping parse(const std::string &yarrrmlFilePath, bool ignoreNonFatalErrors = true) override;
};

} // namespace yarrrml
