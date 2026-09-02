#include "r2rml/ColumnTermMap.h"
#include "r2rml/SerdTerm.h"
#include "r2rml/SQLRow.h"
#include "r2rml/SQLValue.h"

#include <ostream>

namespace r2rml {

ColumnTermMap::ColumnTermMap(const std::string &column) : columnName(column) {
}

ColumnTermMap::~ColumnTermMap() = default;

void ColumnTermMap::generateRDFTerm(const SQLRow &row, rdf::Term &out) const {
	out.clear();
	auto val = row.getValue(columnName);
	if (val->isNull()) {
		return; // NULL column - no term for this row
	}

	// R2RML 7.4's three term types. rr:BlankNode takes the column's value as
	// the blank node identifier; per the spec the mapping is responsible for
	// that value being a valid one.
	//
	// The datatype deliberately does NOT go on the term here: computeDatatypeIRI
	// is the caller's business, because it applies only in the object position -
	// folding it in would start annotating subjects and graph terms too.
	out.mutableLexical() = val->asString();
	out.setKind(kindForTermType(termType));
}

std::string ColumnTermMap::computeDatatypeIRI(const SQLRow &row) const {
	// Static rr:datatype in the mapping takes priority over inferred types.
	if (datatypeIRI) {
		return *datatypeIRI;
	}
	auto val = row.getValue(columnName);
	if (!val || val->isNull()) {
		return std::string();
	}
	return val->datatypeIRI();
}

std::ostream &ColumnTermMap::print(std::ostream &os) const {
	os << "ColumnTermMap { column=\"" << columnName << "\" ";
	TermMap::print(os);
	os << " }";
	return os;
}

} // namespace r2rml
