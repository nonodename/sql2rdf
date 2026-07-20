#include "r2rml/AbstractMap.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace r2rml {

AbstractMap::~AbstractMap() = default;

std::string AbstractMap::percentEncode(const std::string &value) {
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

void AbstractMap::checkWriteStatus(SerdStatus status) {
	if (status != SERD_SUCCESS) {
		throw std::runtime_error(std::string("R2RML: failed to write RDF statement: ") +
		                         reinterpret_cast<const char *>(serd_strerror(status)));
	}
}

} // namespace r2rml
