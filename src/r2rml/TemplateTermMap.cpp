#include "r2rml/TemplateTermMap.h"
#include "r2rml/SerdTerm.h"
#include "r2rml/SQLRow.h"
#include "r2rml/SQLValue.h"

#include <ostream>
#include <string>

namespace r2rml {

TemplateTermMap::TemplateTermMap(const std::string &templ) : templateString(templ) {
}

TemplateTermMap::~TemplateTermMap() = default;

void TemplateTermMap::generateRDFTerm(const SQLRow &row, rdf::Term &out) const {
	// A template term map is an IRI unless rr:termType says otherwise (R2RML
	// 7.4); the rr:BlankNode case takes the expanded string as the blank node
	// identifier. R2RML 7.3 only prescribes percent-encoding of substituted
	// values for rr:IRI: applying it to rr:Literal would corrupt the lexical
	// form, and to rr:BlankNode could emit a '%' that BLANK_NODE_LABEL forbids.
	const rdf::TermKind kind = kindForTermType(termType);
	const bool shouldPercentEncode = (kind == rdf::TermKind::Iri);

	// clear() first, then append, then setKind() - the contract for building a
	// term piecewise. clear() keeps the buffer's capacity, so expanding a
	// template row after row does not reallocate; that is what replaces the
	// mutable expanded_ member this used to append into.
	out.clear();
	std::string &expanded = out.mutableLexical();

	// Expand {COLUMN} placeholders from the row.
	std::size_t i = 0;
	const std::size_t n = templateString.size();
	while (i < n) {
		if (templateString[i] == '{') {
			std::size_t end = templateString.find('}', i + 1);
			if (end == std::string::npos) {
				break; // malformed template - treat rest as literal
			}
			std::string colName = templateString.substr(i + 1, end - i - 1);
			auto val = row.getValue(colName);
			if (val->isNull()) {
				out.clear();
				return; // required column is missing/null - no term for this row
			}
			expanded += shouldPercentEncode ? percentEncode(val->asString()) : val->asString();
			i = end + 1;
		} else {
			expanded += templateString[i];
			++i;
		}
	}

	out.setKind(kind);
}

std::ostream &TemplateTermMap::print(std::ostream &os) const {
	os << "TemplateTermMap { template=\"" << templateString << "\" ";
	TermMap::print(os);
	os << " }";
	return os;
}

} // namespace r2rml
