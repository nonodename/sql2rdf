#pragma once

#include <string>

namespace r2rml {
class R2RMLMapping;
} // namespace r2rml

namespace yarrrml {

/**
 * Translates YARRRML (https://rml.io/yarrrml/spec/) YAML mapping documents
 * into R2RML Turtle and parses the result into the r2rml object model.
 *
 * Internally this translates the supported YARRRML subset into an R2RML
 * Turtle document and reuses r2rml::R2RMLParser::parseString() to build the
 * object model, so all downstream engine behaviour (term-map generation,
 * joins, datatypes, etc.) is identical between R2RML and YARRRML mappings.
 */
class YARRRMLParser {
public:
	YARRRMLParser() = default;
	~YARRRMLParser() = default;

	/**
	 * Translate a YARRRML YAML document (text) to an R2RML Turtle document.
	 *
	 * Throws std::runtime_error on fatal problems (YAML syntax errors, a
	 * missing `mappings` key, etc). Non-fatal issues (unsupported keys,
	 * skipped graphs, multiple sources, etc) are silently dropped; use
	 * parse() to have them reported via R2RMLMapping::parseErrors.
	 */
	std::string translateToTurtle(const std::string &yamlText);

	/**
	 * Read `yarrrmlFilePath`, translate it to Turtle, and parse the result
	 * into the R2RML object model.
	 *
	 * @param ignoreNonFatalErrors
	 *   - true  (default): non-fatal issues found either during YARRRML
	 *     translation (unsupported keys, skipped graphs, multiple sources,
	 *     unresolved condition functions, ...) or during the underlying
	 *     R2RML parse are collected into the returned mapping's parseErrors.
	 *   - false: any such non-fatal issue causes a std::runtime_error to be
	 *     thrown instead.
	 *
	 * Fatal problems (unreadable file, YAML syntax error, missing `mappings`
	 * key) always throw std::runtime_error, regardless of this flag.
	 */
	r2rml::R2RMLMapping parse(const std::string &yarrrmlFilePath, bool ignoreNonFatalErrors = true);

	/**
	 * Convenience: read `yarrrmlFilePath` and translate it to an R2RML Turtle
	 * document (equivalent to translateToTurtle() on the file's contents).
	 */
	std::string translateFileToTurtle(const std::string &yarrrmlFilePath);
};

} // namespace yarrrml
