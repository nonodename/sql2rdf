#include "sparql2sql/TermInfo.h"

namespace sparql2sql {

namespace xsd {

const char *const kInteger = "http://www.w3.org/2001/XMLSchema#integer";
const char *const kDecimal = "http://www.w3.org/2001/XMLSchema#decimal";
const char *const kDouble = "http://www.w3.org/2001/XMLSchema#double";
const char *const kFloat = "http://www.w3.org/2001/XMLSchema#float";
const char *const kString = "http://www.w3.org/2001/XMLSchema#string";
const char *const kBoolean = "http://www.w3.org/2001/XMLSchema#boolean";
const char *const kDateTime = "http://www.w3.org/2001/XMLSchema#dateTime";
const char *const kDate = "http://www.w3.org/2001/XMLSchema#date";
const char *const kTime = "http://www.w3.org/2001/XMLSchema#time";
const char *const kLong = "http://www.w3.org/2001/XMLSchema#long";
const char *const kInt = "http://www.w3.org/2001/XMLSchema#int";
const char *const kShort = "http://www.w3.org/2001/XMLSchema#short";
const char *const kByte = "http://www.w3.org/2001/XMLSchema#byte";
const char *const kHexBinary = "http://www.w3.org/2001/XMLSchema#hexBinary";

} // namespace xsd

const char *const kRdfLangString = "http://www.w3.org/1999/02/22-rdf-syntax-ns#langString";

bool isXsdIntegralIri(const std::string &iri) {
	// The narrower subtypes are pure aliases of xsd:integer throughout this
	// translator (no range clamping), matching translateXsdCast's stance.
	return iri == xsd::kInteger || iri == xsd::kLong || iri == xsd::kInt || iri == xsd::kShort || iri == xsd::kByte;
}

bool isXsdNumericIri(const std::string &iri) {
	return isXsdIntegralIri(iri) || iri == xsd::kDecimal || iri == xsd::kDouble || iri == xsd::kFloat;
}

bool isXsdTemporalIri(const std::string &iri) {
	return iri == xsd::kDate || iri == xsd::kDateTime;
}

bool isXsdCastIri(const std::string &iri) {
	return isXsdNumericIri(iri) || iri == xsd::kString || iri == xsd::kBoolean || isXsdTemporalIri(iri);
}

bool TermInfo::isNumeric() const {
	return kind == RdfTermKind::Literal && isXsdNumericIri(datatypeIri);
}

bool TermInfo::isIntegral() const {
	return kind == RdfTermKind::Literal && isXsdIntegralIri(datatypeIri);
}

bool TermInfo::isTemporal() const {
	return kind == RdfTermKind::Literal && isXsdTemporalIri(datatypeIri);
}

bool TermInfo::isStringy() const {
	return kind == RdfTermKind::Literal && (datatypeIri == xsd::kString || datatypeIri == kRdfLangString);
}

TermInfo meet(const TermInfo &a, const TermInfo &b) {
	TermInfo out;
	out.kind = (a.kind == b.kind) ? a.kind : RdfTermKind::Unknown;
	if (out.kind == RdfTermKind::Unknown) {
		// Unknown is absorbing all the way down: without a trustworthy kind
		// there is no trustworthy datatype or language either.
		return out;
	}
	if (a.datatypeIri == b.datatypeIri) {
		out.datatypeIri = a.datatypeIri;
	}
	if (a.lang == b.lang) {
		out.lang = a.lang;
	}
	return out;
}

} // namespace sparql2sql
