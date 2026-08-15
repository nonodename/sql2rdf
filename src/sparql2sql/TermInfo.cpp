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

const char *const kTagIri = "I";
const char *const kTagBlankNode = "B";
const char *const kTagLiteralUntyped = "L";
const char kTagDatatypePrefix = 'D';
const char kTagLangPrefix = '@';

bool isFullyDetermined(const TermInfo &info) {
	if (info.degraded) {
		// Something below this annotation disagreed, so whatever survived in the
		// three fields is a lower bound rather than a description of the term. In
		// particular a dropped datatype must not be lowered to kTagLiteralUntyped,
		// which would claim the datatype is unknowable when it is merely per-row.
		return false;
	}
	switch (info.kind) {
	case RdfTermKind::Unknown:
		return false;
	case RdfTermKind::Iri:
	case RdfTermKind::BlankNode:
		return true;
	case RdfTermKind::Literal:
		break;
	}
	if (!info.lang.empty()) {
		return true;
	}
	if (info.datatypeIri == kRdfLangString) {
		// Known language-tagged, specific tag not known: exactly the meet of two
		// arms declaring different rr:language values.
		return false;
	}
	if (!info.datatypeIri.empty()) {
		return true;
	}
	// No datatype and no language. Determined only if no contributing arm was
	// language-tagged - otherwise a tagged arm met an untagged one and the two
	// are indistinguishable without a per-row answer.
	return !info.maybeLangTagged;
}

std::string encodeTag(const TermInfo &info) {
	if (!isFullyDetermined(info)) {
		return std::string();
	}
	switch (info.kind) {
	case RdfTermKind::Iri:
		return kTagIri;
	case RdfTermKind::BlankNode:
		return kTagBlankNode;
	case RdfTermKind::Unknown:
		return std::string(); // unreachable: isFullyDetermined rejected it
	case RdfTermKind::Literal:
		break;
	}
	if (!info.lang.empty()) {
		return kTagLangPrefix + info.lang;
	}
	if (!info.datatypeIri.empty()) {
		return kTagDatatypePrefix + info.datatypeIri;
	}
	return kTagLiteralUntyped;
}

TermInfo decodeTag(const std::string &tag) {
	TermInfo out;
	if (tag.empty()) {
		return out;
	}
	if (tag == kTagIri) {
		out.kind = RdfTermKind::Iri;
		return out;
	}
	if (tag == kTagBlankNode) {
		out.kind = RdfTermKind::BlankNode;
		return out;
	}
	if (tag == kTagLiteralUntyped) {
		out.kind = RdfTermKind::Literal;
		return out;
	}
	if (tag[0] == kTagLangPrefix) {
		out.kind = RdfTermKind::Literal;
		out.lang = tag.substr(1);
		out.datatypeIri = kRdfLangString;
		out.maybeLangTagged = true;
		return out;
	}
	if (tag[0] == kTagDatatypePrefix) {
		out.kind = RdfTermKind::Literal;
		out.datatypeIri = tag.substr(1);
		return out;
	}
	return out;
}

TermInfo meet(const TermInfo &a, const TermInfo &b) {
	TermInfo out;
	// Sticky record of "something actually disagreed somewhere below", kept
	// separately from the three degraded fields because their degraded state is
	// textually indistinguishable from never having been declared at all.
	out.degraded = a.degraded || b.degraded || a.kind != b.kind || a.datatypeIri != b.datatypeIri || a.lang != b.lang;
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
	out.maybeLangTagged = a.maybeLangTagged || b.maybeLangTagged;
	return out;
}

} // namespace sparql2sql
