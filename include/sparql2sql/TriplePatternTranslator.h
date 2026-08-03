#pragma once

#include <string>
#include <vector>

#include "sparql-parser/ast/GraphPattern.h"
#include "sparql2sql/TranslatedPattern.h"
#include "sparql2sql/ir/RelNode.h"

namespace sparql2sql {

/// A triple-pattern subject/object position: either a variable or a bound
/// constant term. Blank nodes are represented as variables named
/// "_bnode_<label>" - scoped like variables during translation, but registered
/// as internal on the TranslationContext so they never reach query output.
struct TermSpec {
	bool isVar = false;
	std::string varName;                          // valid iff isVar
	const sparql::ast::Term *boundTerm = nullptr; // valid iff !isVar
};

/// Build the TermSpec for a subject/object term, registering a blank node's
/// synthesized variable as internal.
TermSpec termSpecFor(const sparql::ast::Term &term, TranslationContext &ctx);

/// Build a TermSpec for an already-minted variable name (used by the property
/// path translator for sequence-path intermediate nodes).
TermSpec varTermSpec(const std::string &varName);

/// The predicate-position constraint of one atomic (single-step) pattern.
/// Property paths are desugared into atomic patterns by PropertyPathTranslator
/// before reaching here; `NotIn` is what a negated property set (`!(:a|:b)`)
/// desugars its forward and inverse halves into.
struct PredicateConstraint {
	enum Kind {
		ConstantIri, ///< a single fixed predicate IRI (or `a`).
		Variable,    ///< a bare variable: matches any predicate, binding it.
		NotIn        ///< matches any predicate other than `excludedIris`.
	};

	Kind kind = ConstantIri;
	std::string iri;                       // valid iff kind == ConstantIri
	std::string varName;                   // valid iff kind == Variable
	std::vector<std::string> excludedIris; // valid iff kind == NotIn
};

PredicateConstraint constantPredicate(const std::string &iri);
PredicateConstraint variablePredicate(const std::string &varName);
PredicateConstraint negatedPredicate(std::vector<std::string> excludedIris);

/// Translate one atomic (single-step) pattern into an IR relation by
/// enumerating every R2RML TriplesMap/PredicateObjectMap/rr:class candidate
/// that could produce a matching triple (the "alpha/beta inversion"; see
/// Chebotko/Lu/Fotouhi's SPARQL-to-SQL translation, adapted since our
/// mapping isn't static - it's derived per triple pattern from the R2RML
/// mapping), generating one UNION branch per surviving candidate.
///
/// Always succeeds - a pattern that provably matches nothing given this
/// mapping translates to a valid, always-empty relation, not a translation
/// error (a real SPARQL engine would just answer with zero rows).
RelNodePtr translateAtomicPattern(const TermSpec &subjectSpec, const PredicateConstraint &predicateSpec,
                                  const TermSpec &objectSpec, TranslationContext &ctx);

/// A relation over every RDF term the mapping can produce as a subject or an
/// object - SPARQL's nodes(G), which deliberately excludes predicates -
/// projected once under each name in `varNames`, so all of its columns hold
/// the same term. Deduplicated: this is a set of terms, not a bag.
///
/// Used by the zero-length property path (`E?`) when both of its endpoints are
/// unbound variables and there is therefore nothing to anchor the identity
/// relation to. Necessarily scans every logical table in the mapping.
RelNodePtr allTermsRelation(const std::vector<std::string> &varNames, TranslationContext &ctx);

/// Translate a single SPARQL triple pattern, of any predicate shape, into an
/// IR relation. A constant IRI or bare variable in predicate position goes
/// straight to translateAtomicPattern; every other property path operator is
/// first desugared by translatePath() (see PropertyPathTranslator.h), which
/// throws TranslationError for the operators that are not yet supported.
RelNodePtr translateTriplePattern(const sparql::ast::TriplePattern &tp, TranslationContext &ctx);

} // namespace sparql2sql
