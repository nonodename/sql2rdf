#pragma once

#include <vector>

#include "sparql-parser/ast/GraphPattern.h"
#include "sparql2sql/TranslatedPattern.h"
#include "sparql2sql/ir/RelNode.h"

namespace sparql2sql {

/// Fold a GroupGraphPattern's flat, ordered `elements` list into a single
/// TranslatedPattern, left to right, per the SPARQL algebra (AND=inner
/// join, OPTIONAL=left outer join, UNION=schema-extending union,
/// MINUS=anti-join, FILTER/BIND=applied against everything bound so far).
///
/// Every element that owns a nested GroupGraphPattern (OptionalGraphPattern,
/// MinusGraphPattern, each UnionGraphPattern branch, SubSelectElement's
/// query, ExistsExpr's pattern, a bare nested GroupGraphPattern) is
/// translated by a fresh, independent recursive fold() call starting from
/// the identity relation - never by threading the outer accumulator's SQL
/// in - so FILTER/BIND scoping (only seeing variables bound strictly before
/// them in the same element list) falls out correctly with one uniform
/// recursive structure.
RelNodePtr fold(const sparql::ast::GroupGraphPattern &pattern, TranslationContext &ctx);

// --- Combinators, exposed for direct unit testing. ---

RelNodePtr identityRelation(TranslationContext &ctx);

/// AND (Rule 8/14): inner join on shared variables. Emits a JoinNode whose
/// EquiKeys carry each shared variable's null-safety (COALESCE + null-tolerant
/// ON) only when either operand could produce NULL for it (paper's
/// simplifications 2/3).
RelNodePtr innerJoin(RelNodePtr left, RelNodePtr right, TranslationContext &ctx);

/// OPTIONAL (Rule 9/15): left outer join. Only `left`'s own guarantee
/// determines whether a shared variable stays definitely-bound (an
/// unmatched left row still keeps its own columns; right's columns are
/// NULLed regardless of right's own optionality history), so this has its
/// own bound/optional bookkeeping distinct from innerJoin's.
RelNodePtr leftOuterJoin(RelNodePtr left, RelNodePtr right, TranslationContext &ctx);

/// MINUS: removes rows of `left` compatible with and sharing >=1 variable
/// with a row of `right`. Per SPARQL 1.1 Section 18.2, if `left` and
/// `right` share zero variables, MINUS is a spec-mandated no-op - `left`
/// is returned completely unchanged, with no anti-join emitted at all.
RelNodePtr antiJoin(RelNodePtr left, RelNodePtr right, TranslationContext &ctx);

/// UNION (Rule 10/16): schema-extending, bag-preserving union of >=1
/// independently-folded branches. Rendered via the dialect's combineByName
/// (DuckDB's UNION ALL BY NAME), which subsumes the paper's manual
/// NULL-padding/simplification-5 logic.
RelNodePtr unionAll(std::vector<RelNodePtr> branches, TranslationContext &ctx);

/// Translate a VALUES block (used both as a GroupGraphPatternSub element -
/// InlineData - and as a Query's trailing ValuesClause; identical shape
/// either way) into a self-contained relation.
RelNodePtr translateInlineData(const sparql::ast::InlineData &values, TranslationContext &ctx);

} // namespace sparql2sql
