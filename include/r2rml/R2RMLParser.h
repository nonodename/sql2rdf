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
};

} // namespace r2rml
