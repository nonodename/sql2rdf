#include "r2rml/MappingParser.h"
#include "r2rml/R2RMLParser.h"
#include "yarrrml/YARRRMLParser.h"

#include <stdexcept>

namespace r2rml {

std::unique_ptr<MappingParser> MappingParser::create(const std::string &mappingFilePath) {
	if (yarrrml::YARRRMLParser::hasYarrrmlExtension(mappingFilePath)) {
		return std::unique_ptr<MappingParser>(new yarrrml::YARRRMLParser());
	}

	static const std::string ttlExt = ".ttl";
	if (mappingFilePath.size() >= ttlExt.size() &&
	    mappingFilePath.compare(mappingFilePath.size() - ttlExt.size(), ttlExt.size(), ttlExt) == 0) {
		return std::unique_ptr<MappingParser>(new R2RMLParser());
	}

	throw std::runtime_error("No parser available for file: " + mappingFilePath +
	                         " (expected .ttl for R2RML or .yml/.yaml/.yarrrml for YARRRML)");
}

} // namespace r2rml
