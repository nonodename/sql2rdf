#include "r2rml/MappingParser.h"
#include "r2rml/R2RMLMapping.h"
#include "r2rml/R2RMLParser.h"
#include "yarrrml/YARRRMLParser.h"

#include <stdexcept>

namespace r2rml {

namespace {
bool hasTtlExtension(const std::string &mappingFilePath) {
	static const std::string ttlExt = ".ttl";
	return mappingFilePath.size() >= ttlExt.size() &&
	       mappingFilePath.compare(mappingFilePath.size() - ttlExt.size(), ttlExt.size(), ttlExt) == 0;
}
} // namespace

std::unique_ptr<MappingParser> MappingParser::create(const std::string &mappingFilePath) {
	if (yarrrml::YARRRMLParser::hasYarrrmlExtension(mappingFilePath)) {
		return std::unique_ptr<MappingParser>(new yarrrml::YARRRMLParser());
	}

	if (hasTtlExtension(mappingFilePath)) {
		return std::unique_ptr<MappingParser>(new R2RMLParser());
	}

	throw std::runtime_error("No parser available for file: " + mappingFilePath +
	                         " (expected .ttl for R2RML or .yml/.yaml/.yarrrml for YARRRML)");
}

R2RMLMapping MappingParser::parseMultiple(const std::vector<std::string> &mappingFilePaths, bool ignoreNonFatalErrors,
                                          bool forceYarrrml) {
	TripleCollector collector;

	for (const std::string &path : mappingFilePaths) {
		collector.beginSource(path);

		if (forceYarrrml || yarrrml::YARRRMLParser::hasYarrrmlExtension(path)) {
			yarrrml::YARRRMLParser().collectFile(path, collector);
		} else if (hasTtlExtension(path)) {
			R2RMLParser().collectFile(path, collector);
		} else {
			throw std::runtime_error("No parser available for file: " + path +
			                         " (expected .ttl for R2RML or .yml/.yaml/.yarrrml for YARRRML)");
		}
	}

	return R2RMLParser().parseCollected(collector, ignoreNonFatalErrors);
}

} // namespace r2rml
