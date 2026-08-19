#pragma once

#include "sparql2sql/ir/RelNode.h"

namespace sparql2sql {

struct TypeCatalog;
class TranslationContext;

/// Knobs the optimizer needs from the surrounding query that aren't derivable
/// from the pattern IR alone.
struct OptimizerOptions {
	/// The enclosing query dedups its result (SELECT DISTINCT) or is an ASK
	/// existence check - so any per-pattern DISTINCT is redundant and may be
	/// dropped (the always-safe DISTINCT-elimination case).
	bool topLevelDistinct = false;

	/// Optional column-type catalog enabling native-typed join keys; nullptr
	/// => keep VARCHAR-cast joins.
	const TypeCatalog *catalog = nullptr;

	/// Optional translation context. When supplied, filter pushdown may render
	/// a pushed conjunct into an SpjRelation's WHERE list instead of wrapping
	/// the block in another FilterNode - which both removes a derived-table
	/// layer and, crucially, keeps the block an SpjRelation so the following
	/// flatten pass can still merge it with its inner-join partners (and so
	/// apply the native-typed-join-key rewrite). nullptr => conjuncts are only
	/// ever re-parented, never folded.
	TranslationContext *ctx = nullptr;
};

/// Apply the fixed pass pipeline to a pattern IR tree and return the rewritten
/// root. Semantics-preserving. Passes, in order: filter pushdown (before
/// flattening, so folded predicates don't sit between joinable SPJ blocks), SPJ
/// flattening, self-join elimination, redundant-predicate removal (a
/// "<x> IS NOT NULL" guard already implied by an "<x> = '<literal>'" conjunct
/// in the same block, and - when a TranslationContext is supplied - a
/// comparison against a column an rr:sqlQuery view's own SELECT list defines
/// as a fixed literal), and (when the enclosing query dedups) DISTINCT
/// stripping.
RelNodePtr optimize(RelNodePtr root, const OptimizerOptions &opts);

/// True when projecting `outputVars` (in order) over `schema` is a bare identity:
/// same variables, same order, nothing renamed/reordered/dropped. Wrapping such a
/// relation in a "SELECT alias.a AS a, alias.b AS b, ... FROM (<relation>) AS alias"
/// derived table - the shape a query-level projection or a `{ SELECT ... }`
/// subquery's own projection otherwise always builds - would be a no-op layer, so a
/// caller that also confirms no DISTINCT/GROUP BY/ORDER BY/LIMIT/tag-column need is
/// riding on that wrap may splice the relation's own SQL in directly instead.
bool isIdentityProjection(const std::vector<ColumnInfo> &schema, const std::vector<std::string> &outputVars);

} // namespace sparql2sql
