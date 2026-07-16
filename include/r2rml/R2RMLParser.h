#pragma once

#include <string>

#include "r2rml/MappingParser.h"

namespace r2rml {

class R2RMLMapping;

/**
 * Parses an R2RML mapping file (typically Turtle) and constructs the
 * corresponding C++ object model.
 */
class R2RMLParser : public MappingParser {
public:
	R2RMLParser();
	~R2RMLParser() override = default;

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
	R2RMLMapping parse(const std::string &mappingFilePath, bool ignoreNonFatalErrors = true) override;

	/**
	 * Parse an in-memory Turtle document into the R2RML object model.
	 *
	 * @param turtleText  The Turtle document text.
	 * @param baseUri     Base URI used to resolve relative references (e.g.
	 *                    "<#TriplesMap1>") within `turtleText`.
	 * @param ignoreNonFatalErrors  See parse().
	 */
	R2RMLMapping parseString(const std::string &turtleText, const std::string &baseUri,
	                         bool ignoreNonFatalErrors = true);

	/**
	 * Build the R2RML object model from statements already gathered in
	 * `collector`, without parsing any Turtle text. Intended for callers
	 * (such as YARRRMLParser) that construct SerdNode-based statements
	 * directly rather than serializing and re-parsing Turtle.
	 *
	 * @param ignoreNonFatalErrors  See parse().
	 */
	R2RMLMapping parseCollected(TripleCollector &collector, bool ignoreNonFatalErrors = true);
};

} // namespace r2rml
