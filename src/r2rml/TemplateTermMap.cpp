#include "r2rml/TemplateTermMap.h"
#include "r2rml/SQLRow.h"
#include "r2rml/SQLValue.h"

#include <ostream>
#include <string>

namespace r2rml {

TemplateTermMap::TemplateTermMap(const std::string &templ) : templateString(templ) {
}

TemplateTermMap::~TemplateTermMap() = default;

SerdNode TemplateTermMap::generateRDFTerm(const SQLRow &row, const SerdEnv & /*env*/) const {
	// Expand {COLUMN} placeholders from the row.
	expanded_.clear();
	std::size_t i = 0;
	const std::size_t n = templateString.size();
	while (i < n) {
		if (templateString[i] == '{') {
			std::size_t end = templateString.find('}', i + 1);
			if (end == std::string::npos) {
				break; // malformed template – treat rest as literal
			}
			std::string colName = templateString.substr(i + 1, end - i - 1);
			auto val = row.getValue(colName);
			if (val->isNull()) {
				return SERD_NODE_NULL; // required column is missing/null
			}
			expanded_ += percentEncode(val->asString());
			i = end + 1;
		} else {
			expanded_ += templateString[i];
			++i;
		}
	}

	// Return a URI node whose buf points into expanded_ (no allocation).
	return serd_node_from_string(SERD_URI, reinterpret_cast<const uint8_t *>(expanded_.c_str()));
}

std::ostream &TemplateTermMap::print(std::ostream &os) const {
	os << "TemplateTermMap { template=\"" << templateString << "\" ";
	TermMap::print(os);
	os << " }";
	return os;
}

} // namespace r2rml
