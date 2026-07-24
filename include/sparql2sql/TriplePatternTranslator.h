#pragma once

#include "sparql-parser/ast/GraphPattern.h"
#include "sparql2sql/TranslatedPattern.h"
#include "sparql2sql/ir/RelNode.h"

namespace sparql2sql {

/// Translate a single SPARQL triple pattern into an IR relation by
/// enumerating every R2RML TriplesMap/PredicateObjectMap/rr:class candidate
/// that could produce a matching triple (the "alpha/beta inversion"; see
/// Chebotko/Lu/Fotouhi's SPARQL-to-SQL translation, adapted since our
/// mapping isn't static - it's derived per triple pattern from the R2RML
/// mapping), generating one UNION ALL branch per surviving candidate.
///
/// Always succeeds - a pattern that provably matches nothing given this
/// mapping translates to a valid, always-empty relation, not a translation
/// error (a real SPARQL engine would just answer with zero rows).
///
/// Only PredicatePath (constant IRI/`a`) and VariablePath (bare variable)
/// are supported in predicate position; any other PathKind throws
/// TranslationError.
RelNodePtr translateTriplePattern(const sparql::ast::TriplePattern &tp, TranslationContext &ctx);

} // namespace sparql2sql
