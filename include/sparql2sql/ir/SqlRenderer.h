#pragma once

#include "sparql2sql/TranslatedPattern.h"

namespace sparql2sql {

class RelNode;

/// Render an IR relation into a self-contained TranslatedPattern: a SQL string
/// valid to wrap as "(<sql>) AS aliasN", plus the bound/optional variable sets
/// derived from the node's schema. Fresh subquery-wrapper aliases are minted
/// via `ctx.nextAlias()`; base-table aliases baked into the IR at build time
/// are reused as-is.
TranslatedPattern renderRelation(const RelNode &node, TranslationContext &ctx);

} // namespace sparql2sql
