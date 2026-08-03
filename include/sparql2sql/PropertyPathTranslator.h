#pragma once

#include <string>
#include <vector>

#include "sparql-parser/ast/Path.h"
#include "sparql2sql/TranslatedPattern.h"
#include "sparql2sql/TriplePatternTranslator.h"
#include "sparql2sql/ir/RelNode.h"

namespace sparql2sql {

/// Translate a property path between two endpoints into an IR relation, by
/// desugaring the path operators into the existing relational algebra exactly
/// as SPARQL 1.1 Section 18.1.7 defines them:
///
///   <p> / `a`   -> one atomic pattern (translateAtomicPattern)
///   ?p          -> one atomic pattern with a variable predicate
///   ^E          -> E with the two endpoints swapped
///   E1/E2       -> inner join of E1 and E2 over a fresh internal variable
///   E1|E2       -> bag-preserving union of both alternatives
///   E?          -> set union of E with the zero-length path
///   !(a|^b)     -> union of a forward and an inverse "predicate not in {...}"
///                  atomic pattern
///
/// `E*` and `E+` are not supported and throw TranslationError naming the
/// operator: unlike the above they are not syntactic sugar over the algebra,
/// needing a recursive-CTE IR node and a matching SqlDialect seam.
RelNodePtr translatePath(const sparql::ast::PropertyPathExpr &path, const TermSpec &subject, const TermSpec &object,
                         TranslationContext &ctx);

/// The zero-length path between two endpoints: the identity relation on RDF
/// terms (SPARQL 1.1 Section 18.4's ZeroLengthPath). Both endpoints bound
/// reduces to a compile-time equality test; one endpoint bound binds the other
/// to that term; two variables range over every term the mapping can produce
/// (allTermsRelation, declared in TriplePatternTranslator.h).
RelNodePtr zeroLengthPath(const TermSpec &subject, const TermSpec &object, TranslationContext &ctx);

} // namespace sparql2sql
