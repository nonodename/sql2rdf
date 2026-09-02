#include "r2rml/ConstantTermMap.h"
#include "r2rml/SerdTerm.h"

#include <ostream>
#include <utility>

namespace r2rml {

ConstantTermMap::ConstantTermMap(const SerdNode &node) {
	// The n_bytes > 0 guard is load-bearing and long-standing: a zero-length
	// rr:constant leaves the term absent, isValid() rejects this term map, and
	// the enclosing predicate-object map emits nothing. An empty literal would
	// otherwise be a perfectly well-formed term and would start producing a
	// triple with an empty value.
	if (node.type != SERD_NOTHING && node.buf && node.n_bytes > 0) {
		constantValue = termFromSerdNode(&node);
	}
}

ConstantTermMap::ConstantTermMap(rdf::Term term) : constantValue(std::move(term)) {
}

ConstantTermMap::~ConstantTermMap() = default;

void ConstantTermMap::generateRDFTerm(const SQLRow &, rdf::Term &out) const {
	out = constantValue;
}

std::ostream &ConstantTermMap::print(std::ostream &os) const {
	os << "ConstantTermMap { value=\"" << constantValue.lexical() << "\" ";
	TermMap::print(os);
	os << " }";
	return os;
}

} // namespace r2rml
