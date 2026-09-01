#pragma once

#include "TermMap.h"
#include <memory>

namespace r2rml {

/**
 * A term map that derives its value from a column in the logical table.
 */
class ColumnTermMap : public TermMap {
public:
	ColumnTermMap() = default;
	explicit ColumnTermMap(const std::string &column);
	~ColumnTermMap() override;

	void generateRDFTerm(const SQLRow &row, rdf::Term &out) const override;

	std::string computeDatatypeIRI(const SQLRow &row) const override;

	bool isValid() const override {
		// columnName must not be empty
		return !columnName.empty();
	}

	std::ostream &print(std::ostream &os) const override;

	std::string columnName;
};

} // namespace r2rml
