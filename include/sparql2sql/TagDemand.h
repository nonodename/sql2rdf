#pragma once

#include "sparql2sql/TranslatedPattern.h"

namespace sparql {
namespace ast {
class Query;
} // namespace ast
} // namespace sparql

namespace sparql2sql {

class RelNode;

/// Decide which variables need a runtime type-tag column materialised beside
/// their lexical-form column, and record them on `ctx` via markNeedsTag().
///
/// The rule, applied uniformly by both passes below:
///
///     mark a variable iff some construct inspects its RDF term dimension
///     AND its static TermInfo does not already determine that dimension.
///
/// Both halves matter. Without the first, every query would pay for tag columns
/// it never reads. Without the second, a well-typed mapping would materialise
/// tags that only ever constant-fold - so a query whose types the mapping pins
/// down generates exactly the SQL it generated before tags existed, which is
/// what makes the existing structural test suite a regression harness for this
/// feature rather than a casualty of it.
///
/// The split into two passes is forced by *when* the two kinds of consumer are
/// rendered:
///
///  - A join key's comparison is fixed during optimize(), because mergeInner
///    folds it into a flattened block's WHERE text. So its demand must be known
///    first.
///  - A FILTER/BIND/ORDER BY expression is rendered late (deferred-expression
///    design), and filter pushdown gets a shot at resolving it per union arm on
///    the way - where the dimension is often statically known even though the
///    arms' meet is not. Deciding its demand *after* optimize() therefore asks a
///    strictly better question, and a predicate pushdown resolves per arm needs
///    no tag column at all.
void markJoinKeyTagNeeds(const RelNode &root, TranslationContext &ctx);

/// The second pass: FILTER/BIND predicates plus the query's own SELECT items,
/// GROUP BY, HAVING, ORDER BY and DISTINCT key. Run after optimize() and before
/// rendering. `query` may be null for a pattern with no query-level modifiers of
/// its own (an EXISTS body).
///
/// "Inspects the dimension" is read generously: any variable appearing anywhere
/// beneath a comparison, an arithmetic operator, a term-kind builtin,
/// sameTerm/STRDT/STRLANG/COALESCE/IF, a MIN/MAX/SUM/SAMPLE aggregate, or an
/// ORDER BY key. Over-marking costs one unused column; under-marking would make
/// a consumer silently degrade, so the bias is deliberate.
void markExpressionTagNeeds(const RelNode &root, const sparql::ast::Query *query, TranslationContext &ctx);

/// A third pass, run *before* `query.where` is folded - unlike the two above,
/// which need the folded/optimized relation tree's schema to gate on
/// isFullyDetermined() and so can only run after fold()/optimize().
///
/// Exists solely so a nested `{ SELECT ... }` sub-select folded partway through
/// `query.where` (PatternFolder's SubSelectElement case) can see the enclosing
/// query's own demand for a tag - from its ORDER BY/HAVING keys, its SELECT-list
/// expressions, or a DISTINCT/REDUCED dedup key - before that sub-select's SQL
/// text is rendered and frozen. Once frozen, an undetermined tag cannot be added
/// back (see RawRelation::providedTagVars and producedTag()'s error in
/// SqlRenderer.cpp): translateQueryPattern's own markJoinKeyTagNeeds/
/// markExpressionTagNeeds calls run on the *outer* query's tree only after every
/// nested sub-select inside it has already folded and rendered, which is too
/// late for this specific case.
///
/// Deliberately coarser than the two passes above: with no schema/scope
/// available yet, it cannot check isFullyDetermined() before marking. To keep
/// that from over-marking an ordinary top-level query (which would materialise
/// an unwanted tag column even when the mapping pins the type down, regressing
/// the "well-typed mapping generates the same SQL as before" property the other
/// two passes preserve), it only marks a variable when some nested sub-select
/// in `query.where` actually projects it bare - the one shape this pass exists
/// to help with. Everything else is left for markExpressionTagNeeds's later,
/// scope-aware pass to decide, as before this pass existed.
///
/// Naturally covers sub-selects nested more than one level deep: this is called
/// from the top of translateQueryPattern for every query, including nested
/// ones, so each level pre-scans its own solution modifiers before folding its
/// own `where`, which may itself recurse into a further-nested sub-select.
void markPreFoldTagNeeds(const sparql::ast::Query &query, TranslationContext &ctx);

} // namespace sparql2sql
