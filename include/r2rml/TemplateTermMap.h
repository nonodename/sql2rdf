#pragma once

#include "TermMap.h"

#include <string>
#include <memory>

namespace r2rml {

/**
 * A term map defined by an RFC 6570-style template string.  Placeholders are
 * filled with column values from the current row.
 */
class TemplateTermMap : public TermMap {
public:
	TemplateTermMap() = default;
	explicit TemplateTermMap(const std::string &templ);
	~TemplateTermMap() override;

	void generateRDFTerm(const SQLRow &row, rdf::Term &out) const override;

	bool isValid() const override {
		// templateString must not be empty
		return !templateString.empty();
	}

	std::ostream &print(std::ostream &os) const override;

	std::string templateString;
};

} // namespace r2rml
