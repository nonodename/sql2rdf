#include "r2rml/ConstantTermMap.h"

#include <ostream>

namespace r2rml {

ConstantTermMap::ConstantTermMap(const SerdNode &node) {
	if (node.type != SERD_NOTHING && node.buf && node.n_bytes > 0) {
		ownedUri_.assign(reinterpret_cast<const char *>(node.buf), node.n_bytes);
		constantValue.buf = reinterpret_cast<const uint8_t *>(ownedUri_.c_str());
		constantValue.n_bytes = node.n_bytes;
		constantValue.n_chars = node.n_chars;
		constantValue.flags = node.flags;
		constantValue.type = node.type;
	}
}
ConstantTermMap::~ConstantTermMap() = default;
SerdNode ConstantTermMap::generateRDFTerm(const SQLRow &, const SerdEnv &) const {
	return constantValue;
}

std::ostream &ConstantTermMap::print(std::ostream &os) const {
	os << "ConstantTermMap { value=\"" << ownedUri_ << "\" ";
	TermMap::print(os);
	os << " }";
	return os;
}

} // namespace r2rml
