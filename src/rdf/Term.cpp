#include "rdf/Term.h"

namespace rdf {

const char *const RDF_LANG_STRING = "http://www.w3.org/1999/02/22-rdf-syntax-ns#langString";

const char *termKindName(TermKind kind) {
	switch (kind) {
	case TermKind::Iri:
		return "Iri";
	case TermKind::BlankNode:
		return "BlankNode";
	case TermKind::Literal:
		return "Literal";
	case TermKind::Unknown:
		break;
	}
	return "Unknown";
}

// ---------------------------------------------------------------------------
// Named constructors
//
// Each delegates to the corresponding assign* mutator so that the invariants
// are established in exactly one place per kind.
// ---------------------------------------------------------------------------

Term Term::iri(const std::string &value) {
	Term t;
	t.assignIri(value);
	return t;
}

Term Term::blankNode(const std::string &label) {
	Term t;
	t.assignBlankNode(label);
	return t;
}

Term Term::literal(const std::string &lexical_form) {
	Term t;
	t.assignLiteral(lexical_form);
	return t;
}

Term Term::typedLiteral(const std::string &lexical_form, const std::string &datatype_iri) {
	Term t;
	t.assignTypedLiteral(lexical_form, datatype_iri);
	return t;
}

Term Term::langLiteral(const std::string &lexical_form, const std::string &language_tag) {
	Term t;
	t.assignLangLiteral(lexical_form, language_tag);
	return t;
}

// ---------------------------------------------------------------------------
// Mutators
// ---------------------------------------------------------------------------

void Term::clear() {
	// Assign rather than clear() the strings so the capacity survives: this is
	// what lets a caller hoist one Term out of a per-row loop and stop
	// allocating after the first few rows.  std::string::clear() also preserves
	// capacity, but spelling it this way keeps the three fields uniform.
	kind_ = TermKind::Unknown;
	lexical_.clear();
	datatype_.clear();
	lang_.clear();
}

void Term::assignIri(const std::string &value) {
	kind_ = TermKind::Iri;
	lexical_ = value;
	datatype_.clear();
	lang_.clear();
}

void Term::assignBlankNode(const std::string &label) {
	kind_ = TermKind::BlankNode;
	lexical_ = label;
	datatype_.clear();
	lang_.clear();
}

void Term::assignLiteral(const std::string &lexical_form) {
	kind_ = TermKind::Literal;
	lexical_ = lexical_form;
	datatype_.clear();
	lang_.clear();
}

void Term::assignTypedLiteral(const std::string &lexical_form, const std::string &datatype_iri) {
	// An empty datatype is "no datatype stated", which is exactly a plain
	// literal.  rdf:langString without a tag is not a well-formed term, so it
	// degrades the same way rather than being stored and later serialised as a
	// datatype IRI that no parser would accept back.
	if (datatype_iri.empty() || datatype_iri == RDF_LANG_STRING) {
		assignLiteral(lexical_form);
		return;
	}
	kind_ = TermKind::Literal;
	lexical_ = lexical_form;
	datatype_ = datatype_iri;
	lang_.clear();
}

void Term::assignLangLiteral(const std::string &lexical_form, const std::string &language_tag) {
	if (language_tag.empty()) {
		assignLiteral(lexical_form);
		return;
	}
	kind_ = TermKind::Literal;
	lexical_ = lexical_form;
	datatype_.clear();
	lang_ = language_tag;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::string Term::effectiveDatatypeIri() const {
	if (!lang_.empty()) {
		return std::string(RDF_LANG_STRING);
	}
	return datatype_;
}

bool Term::operator==(const Term &other) const {
	if (kind_ != other.kind_ || lexical_ != other.lexical_) {
		return false;
	}
	if (kind_ != TermKind::Literal) {
		return true;
	}
	// Compare the EFFECTIVE datatype so that the abstract term drives equality
	// rather than the representation.  The invariants make this equivalent to
	// comparing the two fields directly today, but it keeps the comparison
	// correct if a future caller ever constructs a langString by datatype.
	return lang_ == other.lang_ && effectiveDatatypeIri() == other.effectiveDatatypeIri();
}

std::ostream &Term::print(std::ostream &os) const {
	switch (kind_) {
	case TermKind::Iri:
		os << "<" << lexical_ << ">";
		break;
	case TermKind::BlankNode:
		os << "_:" << lexical_;
		break;
	case TermKind::Literal:
		os << "\"" << lexical_ << "\"";
		if (!lang_.empty()) {
			os << "@" << lang_;
		} else if (!datatype_.empty()) {
			os << "^^<" << datatype_ << ">";
		}
		break;
	case TermKind::Unknown:
		os << "(null)";
		break;
	}
	return os;
}

std::ostream &operator<<(std::ostream &os, const Term &term) {
	return term.print(os);
}

} // namespace rdf
