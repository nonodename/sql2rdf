#pragma once

#include "sparql2sql/ir/RelNode.h"

namespace sparql2sql {

struct TypeCatalog;

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
};

/// Apply the fixed pass pipeline to a pattern IR tree and return the rewritten
/// root. Pure IR->IR; semantics-preserving. In Phase 1 this is the identity
/// (no passes wired yet).
RelNodePtr optimize(RelNodePtr root, const OptimizerOptions &opts);

} // namespace sparql2sql
