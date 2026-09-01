#include "r2rml/AbstractMap.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace r2rml {

AbstractMap::~AbstractMap() = default;

std::string AbstractMap::percentEncode(const std::string &value) {
	// 1. Fast hex lookup table to replace snprintf
	static const char hexDigits[] = "0123456789ABCDEF";

	// 2. O(1) boolean lookup array to replace std::strchr
	static const bool unreserved[256] = {
	    /* 0-31: Control chars */
	    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	    /* 32-47: Space to / */
	    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, // '-' is 45, '.' is 46
	    /* 48-63: 0-9 and symbols */
	    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,
	    /* 64-95: @ and A-Z, [\]^_ */
	    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, // '_' is 95
	    /* 96-127: ` and a-z, {|}~ */
	    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 0, // '~' is 126
	    /* 128-255: Extended ASCII / UTF-8 bytes (always encoded) */
	    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

	// 3. Exact pre-allocation scan (removes dynamic reallocation and float math)
	size_t requiredSize = 0;
	for (unsigned char c : value) {
		requiredSize += unreserved[c] ? 1 : 3;
	}

	// If no encoding is needed, return early via copy to trigger NRVO
	if (requiredSize == value.size()) {
		return value;
	}

	std::string out;
	out.reserve(requiredSize);

	// 4. Fast conversion loop
	for (unsigned char c : value) {
		if (unreserved[c]) {
			out.push_back(static_cast<char>(c));
		} else {
			out.push_back('%');
			out.push_back(hexDigits[c >> 4]);   // Upper nibble
			out.push_back(hexDigits[c & 0x0F]); // Lower nibble
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
