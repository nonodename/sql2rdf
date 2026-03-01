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

	R2RMLMapping parse(const std::string &mappingFilePath);
};

} // namespace r2rml
