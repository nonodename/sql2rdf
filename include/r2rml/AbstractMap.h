#pragma once

#include <ostream>
#include <string>

#include <serd/serd.h>

namespace r2rml {

/**
 * Common base for the R2RML mapping-model classes (term maps, predicate-object
 * maps, triples maps). Provides the shared human-readable printing contract
 * plus helpers used when generating RDF terms and writing RDF statements.
 */
class AbstractMap {
public:
	virtual ~AbstractMap();

	/**
	 * Write a human-readable representation to the given stream.
	 */
	virtual std::ostream &print(std::ostream &os) const = 0;

protected:
	/// Percent-encode a string per RFC 3986 unreserved-character rules
	/// (encodes all bytes that are not A-Z a-z 0-9 - _ . ~).
	static std::string percentEncode(const std::string &value);

	/// Throw std::runtime_error if writing an RDF statement via Serd failed.
	static void checkWriteStatus(SerdStatus status);
};

} // namespace r2rml
