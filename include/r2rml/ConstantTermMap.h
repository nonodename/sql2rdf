#pragma once

#include "TermMap.h"

#include <string>
#include <memory>

namespace r2rml {

/**
 * A term map which always produces a fixed RDF term.
 *
 * The SerdNode constructor deep-copies the node into an owning rdf::Term, so
 * instances remain valid after the source buffer is freed - which is what the
 * hand-rolled ownedUri_ sidecar used to arrange by hand.
 */
class ConstantTermMap : public TermMap {
public:
	ConstantTermMap() = default;
	explicit ConstantTermMap(const SerdNode &node);
	explicit ConstantTermMap(rdf::Term term);
	~ConstantTermMap() override;

	void generateRDFTerm(const SQLRow &row, rdf::Term &out) const override;

	bool isValid() const override {
		// An absent term means there was no usable rr:constant. Note this also
		// rejects an EMPTY constant: the SerdNode constructor refuses a
		// zero-length node, preserving the long-standing behaviour that
		// rr:constant "" produces no triple at all.
		return !constantValue.isNull();
	}

	std::ostream &print(std::ostream &os) const override;

	rdf::Term constantValue;
};

} // namespace r2rml
