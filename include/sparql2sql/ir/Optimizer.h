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
/// flattening, self-join elimination, and (when the enclosing query dedups)
/// DISTINCT stripping.
RelNodePtr optimize(RelNodePtr root, const OptimizerOptions &opts);

} // namespace sparql2sql
