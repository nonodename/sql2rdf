#include "r2rml/TemplateTermMap.h"
#include "r2rml/SQLRow.h"
#include "r2rml/SQLValue.h"

#include <cstdio>
#include <cstring>
#include <ostream>
#include <string>

namespace r2rml {

namespace {

/// Percent-encode a string per RFC 3986 unreserved-character rules.
/// Encodes all bytes that are not unreserved (A-Z a-z 0-9 - _ . ~).
std::string percentEncode(const std::string &value) {
	static const char unreserved[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	                                 "abcdefghijklmnopqrstuvwxyz"
	                                 "0123456789-_.~";
	std::string out;
	out.reserve(value.size());
	for (unsigned char c : value) {
		if (std::strchr(unreserved, static_cast<char>(c))) {
			out += static_cast<char>(c);
		} else {
			char buf[4];
			std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned>(c));
			out += buf;
		}
	}
	return out;
}

} // anonymous namespace

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
			SQLValue val = row.getValue(colName);
			if (val.isNull()) {
				return SERD_NODE_NULL; // required column is missing/null
			}
			expanded_ += percentEncode(val.asString());
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
