#pragma once

#include <string>
#include <memory>

namespace r2rml {

class R2RMLMapping;

/**
 * Parses an R2RML mapping file (typically Turtle) and constructs the
 * corresponding C++ object model.
 */
class R2RMLParser {
public:
	R2RMLParser();
	~R2RMLParser() = default;

	/**
	 * Parse the R2RML mapping at `mappingFilePath`.
	 *
	 * @param ignoreNonFatalErrors
	 *   - true  (default): logical parse errors (unresolved parentTriplesMap,
	 *     unrecognised logical-table type, unknown objectMap property) are
	 *     collected silently and stored in R2RMLMapping::parseErrors.  They
	 *     are reported when operator<< is called on the returned mapping.
	 *   - false: the first batch of logical parse errors causes a
	 *     std::runtime_error to be thrown instead.
	 */
	R2RMLMapping parse(const std::string &mappingFilePath, bool ignoreNonFatalErrors = true);

	/**
	 * Parse an in-memory Turtle document (e.g. produced by translating a
	 * YARRRML document) into the R2RML object model.
	 *
	 * @param turtleText  The Turtle document text.
	 * @param baseUri     Base URI used to resolve relative references (e.g.
	 *                    "<#TriplesMap1>") within `turtleText`.
	 * @param ignoreNonFatalErrors  See parse().
	 */
	R2RMLMapping parseString(const std::string &turtleText, const std::string &baseUri,
	                        bool ignoreNonFatalErrors = true);
};

} // namespace r2rml
