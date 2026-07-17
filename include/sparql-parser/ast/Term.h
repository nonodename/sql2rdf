#pragma once

#include <memory>
#include <string>

namespace sparql {
namespace ast {

enum class TermKind { Iri, Var, BlankNode, Literal };

/// Base of the RDF-term hierarchy used for subjects/predicates/objects and
/// as expression leaves: Iri, Var, BlankNode, RdfLiteral.
class Term {
public:
	explicit Term(TermKind kind) : kind_(kind) {
	}
	virtual ~Term() = default;

	TermKind kind() const {
		return kind_;
	}

private:
	TermKind kind_;
};

/// An IRI. `value` is the resolved form (prefixed names expanded via the
/// prologue's PREFIX map; relative IRIs resolved against BASE for the
/// common cases only - see Parser.h for the exact scope). `lexicalForm` is
/// the original text as written, kept for the pretty printer.
class Iri : public Term {
public:
	Iri(std::string value, std::string lexicalForm)
	    : Term(TermKind::Iri), value(std::move(value)), lexicalForm(std::move(lexicalForm)) {
	}

	std::string value;
	std::string lexicalForm;
};

class Var : public Term {
public:
	explicit Var(std::string name) : Term(TermKind::Var), name(std::move(name)) {
	}

	std::string name;
};

class BlankNode : public Term {
public:
	explicit BlankNode(std::string label, bool anonymous = false)
	    : Term(TermKind::BlankNode), label(std::move(label)), anonymous(anonymous) {
	}

	std::string label;
	bool anonymous;
};

/// An RDF literal: a lexical form plus at most one of a language tag or a
/// datatype IRI. Numeric/boolean literal sugar (§4.1.2) is normalized here
/// by giving `datatype` the corresponding xsd: IRI at parse time.
class RdfLiteral : public Term {
public:
	explicit RdfLiteral(std::string lexicalForm) : Term(TermKind::Literal), lexicalForm(std::move(lexicalForm)) {
	}

	std::string lexicalForm;
	std::string languageTag;       // empty if none
	std::unique_ptr<Iri> datatype; // null if simple literal (no datatype, no language tag)
};

} // namespace ast
} // namespace sparql
